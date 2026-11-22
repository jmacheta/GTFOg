// ADXL345 over raw I2C: 100 Hz, +-16 g full resolution, FIFO in stream mode. Powered only while a phone subscribes to
// the stream or the fan follows movement; samples go to Bluetooth and to the motion detector.
#include "app.hpp"
#include "motion.hpp"

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

#include <array>
#include <atomic>
#include <chrono>
#include <optional>
#include <tuple>

LOG_MODULE_REGISTER(accelerometer, LOG_LEVEL_INF);

using gtfog::ble_accelerometer_samples_per_packet, gtfog::ble_accelerometer_send, gtfog::with_controller;

namespace {

constexpr std::uint8_t rate_hz = 100;
constexpr std::uint8_t reg_devid = 0x00, reg_bw_rate = 0x2c, reg_power_ctl = 0x2d, reg_data_format = 0x31, reg_datax0 = 0x32, reg_fifo_ctl = 0x38,
                       reg_fifo_status = 0x39;

i2c_dt_spec const bus = I2C_DT_SPEC_GET(DT_NODELABEL(accelerometer));
gpio_dt_spec const power = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), accelerometer_pwr_gpios);

constexpr std::chrono::milliseconds poll_time{200}; // The 32-sample FIFO holds 320 ms at 100 Hz.
constexpr auto shutdown_wait = K_SECONDS(1);        // Longest wait for the thread to cut the power.
constexpr auto retry_delay = K_SECONDS(10);
constexpr int sensor_power_cycle_off_ms = 100;   // With the supply off.
constexpr int sensor_settle_ms = 500;            // From supply on until the sensor answers.
constexpr auto lost_retry_delay = K_SECONDS(60); // A sensor that was given up on is tried again this rarely.

std::atomic<bool> streaming{false};
std::atomic<bool> motion_mode{false};
std::atomic<bool> shutdown{false}; // Set before System OFF; the thread then drops the power pin and stays down.
K_SEM_DEFINE(wanted_changed, 0, 1);
K_SEM_DEFINE(shutdown_done, 0, 1);

auto wanted() -> bool { return !shutdown && (streaming || motion_mode); }

// Waits for the next retry; a give from before the call does not cut the wait short. Only a later change does.
void wait_before_retry(k_timeout_t delay) {
  k_sem_reset(&wanted_changed);
  if(wanted()) {
    std::ignore = k_sem_take(&wanted_changed, delay); // A timeout is the normal end.
  }
}

// A pin write that fails leaves the sensor as it was; nothing else to do.
void power_down() { std::ignore = gpio_pin_configure_dt(&power, GPIO_OUTPUT_INACTIVE); }

auto power_up() -> int {
  if(int err = gpio_pin_configure_dt(&power, GPIO_OUTPUT_INACTIVE); err != 0) {
    return err;
  }
  k_msleep(sensor_power_cycle_off_ms);
  if(int err = gpio_pin_set_dt(&power, 1); err != 0) {
    return err;
  }
  k_msleep(sensor_settle_ms);

  std::uint8_t id = 0;
  if(int err = i2c_reg_read_byte_dt(&bus, reg_devid, &id); err != 0) {
    return err;
  }
  if(id != 0xe5) {
    return -ENODEV;
  }
  int err = i2c_reg_write_byte_dt(&bus, reg_power_ctl, 0x00);      // Standby while configuring.
  err = err ?: i2c_reg_write_byte_dt(&bus, reg_data_format, 0x0b); // Full resolution, +-16 g.
  err = err ?: i2c_reg_write_byte_dt(&bus, reg_bw_rate, 0x0a);     // 100 Hz.
  err = err ?: i2c_reg_write_byte_dt(&bus, reg_fifo_ctl, 0x80);    // FIFO stream mode.
  return err ?: i2c_reg_write_byte_dt(&bus, reg_power_ctl, 0x08);  // Measure.
}

void flush_fifo() {
  // A missed flush only repeats or drops a few samples; a dead sensor is caught by the watchdog.
  std::ignore = i2c_reg_write_byte_dt(&bus, reg_fifo_ctl, 0x00);
  std::ignore = i2c_reg_write_byte_dt(&bus, reg_fifo_ctl, 0x80);
}

void sensor_gone() {
  LOG_ERR("accelerometer lost");
  with_controller([](gtfog::controller &control) { control.sensor_lost(); });
}

void accelerometer_thread(void *, void *, void *) {
  std::array<gtfog::accelerometer_sample, gtfog::accelerometer_max_samples> batch{};
  std::array<std::uint8_t, gtfog::accelerometer_header_size + gtfog::accelerometer_sample_size * gtfog::accelerometer_max_samples> packet{};

  using verdict = gtfog::fifo_watchdog::verdict;
  gtfog::fifo_watchdog watchdog;
  while(true) {
    if(!wanted()) {
      power_down(); // The thread owns the pin.
      if(shutdown) {
        k_sem_give(&shutdown_done);
      }
      k_sem_take(&wanted_changed, K_FOREVER);
      continue;
    }
    if(int err = power_up(); err != 0) {
      LOG_ERR("power up: %d", err);
      power_down();
      auto verdict_now = watchdog.poll(false, gtfog::fifo_watchdog::stall_time);
      if(verdict_now == verdict::lost) {
        sensor_gone();
      }
      wait_before_retry(verdict_now == verdict::lost || verdict_now == verdict::still_lost ? lost_retry_delay : retry_delay);
      continue;
    }
    gtfog::activity_classifier classifier;
    std::uint16_t seq = 0;
    std::size_t filled = 0;
    bool was_streaming = false;
    while(wanted()) {
      if(streaming && !was_streaming) {
        flush_fifo();
        classifier.reset();
        seq = 0;
        filled = 0;
      }
      was_streaming = streaming;
      std::size_t per_packet = streaming ? ble_accelerometer_samples_per_packet() : 0;
      std::optional<gtfog::activity_level> level;
      std::uint8_t status = 0;
      bool healthy = i2c_reg_read_byte_dt(&bus, reg_fifo_status, &status) == 0 && gtfog::fifo_entries(status) != 0;
      if(gtfog::fifo_overflowed(status)) {
        LOG_WRN("FIFO overflow");
        flush_fifo(); // Samples were lost; start over.
        classifier.reset();
        status = 0;
      }
      for(unsigned i = 0; i < gtfog::fifo_entries(status); ++i) {
        std::array<std::uint8_t, 6> raw{};
        if(i2c_burst_read_dt(&bus, reg_datax0, raw.data(), raw.size()) != 0) {
          healthy = false;
          continue;
        }
        gtfog::accelerometer_sample sample{static_cast<std::int16_t>(raw[0] | raw[1] << 8), static_cast<std::int16_t>(raw[2] | raw[3] << 8),
                                           static_cast<std::int16_t>(raw[4] | raw[5] << 8)};
        if(auto done = classifier.update(std::span{&sample, 1})) {
          level = done;
        }
        if(per_packet == 0) {
          continue;
        }
        batch[filled++] = sample;
        if(filled >= per_packet) {
          auto len = gtfog::pack_accelerometer_samples(seq++, rate_hz, std::span{batch.data(), filled}, packet);
          ble_accelerometer_send(std::span{packet.data(), len});
          filled = 0;
          per_packet = ble_accelerometer_samples_per_packet();
        }
      }
      if(motion_mode && level) {
        with_controller([&](gtfog::controller &control) { control.observe(*level); });
      }
      auto verdict_now = watchdog.poll(healthy, poll_time);
      if(verdict_now == verdict::lost) {
        sensor_gone();
      }
      if(verdict_now != verdict::ok) {
        LOG_WRN("no samples, power cycling the sensor");
        if(verdict_now == verdict::lost || verdict_now == verdict::still_lost) {
          power_down();
          wait_before_retry(lost_retry_delay);
        }
        break; // Back to power_up().
      }
      k_msleep(poll_time.count());
    }
  }
}

K_THREAD_DEFINE(accelerometer, 1536, accelerometer_thread, nullptr, nullptr, nullptr, 10, 0, 100);

} // namespace

void gtfog::accelerometer_stream(bool on) {
  streaming = on;
  k_sem_give(&wanted_changed);
}

void gtfog::accelerometer_motion_mode(bool on) {
  motion_mode = on;
  k_sem_give(&wanted_changed);
}

void gtfog::accelerometer_power_off() {
  streaming = false;
  motion_mode = false;
  shutdown = true;
  k_sem_give(&wanted_changed);
  // The thread may be inside power_up() (0.6 s) or wait for the controller mutex held by our caller; do not wait for ever.
  std::ignore = k_sem_take(&shutdown_done, shutdown_wait);
  power_down(); // Fallback when the thread did not answer in time.
}
