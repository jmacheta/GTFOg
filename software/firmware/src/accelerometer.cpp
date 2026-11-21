// ADXL345 over raw I2C: 100 Hz, +-16 g full resolution, FIFO in stream mode. Samples go to Bluetooth while a phone
// subscribes to the stream. Fan control from movement is not implemented yet.
#include "accel.hpp"
#include "app.hpp"

#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

#include <array>
#include <atomic>

LOG_MODULE_REGISTER(accelerometer, LOG_LEVEL_INF);

namespace {

constexpr std::uint8_t rate_hz = 100;
constexpr std::uint8_t reg_devid = 0x00, reg_bw_rate = 0x2c, reg_power_ctl = 0x2d, reg_data_format = 0x31, reg_datax0 = 0x32, reg_fifo_ctl = 0x38,
                       reg_fifo_status = 0x39;

i2c_dt_spec const bus = I2C_DT_SPEC_GET(DT_NODELABEL(accelerometer));
gpio_dt_spec const power = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), accelerometer_pwr_gpios);

std::atomic<bool> streaming{false};
K_SEM_DEFINE(stream_started, 0, 1);

auto power_up() -> int {
  if(int err = gpio_pin_configure_dt(&power, GPIO_OUTPUT_INACTIVE); err != 0) {
    return err;
  }
  k_msleep(100);
  gpio_pin_set_dt(&power, 1);
  k_msleep(500);

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
  std::ignore = i2c_reg_write_byte_dt(&bus, reg_fifo_ctl, 0x00);
  std::ignore = i2c_reg_write_byte_dt(&bus, reg_fifo_ctl, 0x80);
}

void accelerometer_thread(void *, void *, void *) {
  if(int err = power_up(); err != 0) {
    LOG_ERR("power up: %d", err);
    return;
  }
  std::array<gtfog::accel_sample, gtfog::accel_max_samples> batch{};
  std::array<std::uint8_t, gtfog::accel_header_size + gtfog::accel_sample_size * gtfog::accel_max_samples> packet{};
  std::uint16_t seq = 0;

  while(true) {
    if(!streaming) {
      k_sem_take(&stream_started, K_FOREVER);
      flush_fifo();
      seq = 0;
    }
    std::size_t filled = 0;
    std::size_t per_packet = ble_accel_samples_per_packet();
    while(streaming && per_packet > 0) {
      std::uint8_t status = 0;
      std::ignore = i2c_reg_read_byte_dt(&bus, reg_fifo_status, &status);
      for(unsigned i = 0; i < (status & 0x3fu); ++i) {
        std::array<std::uint8_t, 6> raw{};
        if(i2c_burst_read_dt(&bus, reg_datax0, raw.data(), raw.size()) != 0) {
          continue;
        }
        batch[filled++] = {static_cast<std::int16_t>(raw[0] | raw[1] << 8), static_cast<std::int16_t>(raw[2] | raw[3] << 8),
                           static_cast<std::int16_t>(raw[4] | raw[5] << 8)};
        if(filled == per_packet) {
          auto len = gtfog::pack_accel(seq++, rate_hz, std::span{batch.data(), filled}, packet);
          ble_accel_send(std::span{packet.data(), len});
          filled = 0;
          per_packet = ble_accel_samples_per_packet();
        }
      }
      k_msleep(100); // The 32-sample FIFO holds 320 ms at 100 Hz.
    }
  }
}

K_THREAD_DEFINE(accelerometer, 1536, accelerometer_thread, nullptr, nullptr, nullptr, 10, 0, 100);

} // namespace

void accelerometer_stream(bool on) {
  streaming = on;
  if(on) {
    k_sem_give(&stream_started);
  }
}
