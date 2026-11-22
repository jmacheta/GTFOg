// Measures the battery and reads the charger status pins.
#include "app.hpp"

#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/logging/log.h>

#include <chrono>
#include <tuple>

LOG_MODULE_REGISTER(battery, LOG_LEVEL_INF);

using gtfog::read_failure_guard, gtfog::ble_battery_changed, gtfog::ble_charger_changed, gtfog::ble_connected, gtfog::with_controller;

namespace {

// ponytail: VBAT reaches AIN4 through a 47k/47k divider (POWER sheet); calibrate against a multimeter.
constexpr int vbat_divider = 2;
constexpr unsigned sample_interval_s = 10;        // The auto-off resolution.
constexpr auto fault_signal_delay = K_SECONDS(2); // Lets the power-on signal finish first.
constexpr std::uint32_t watchdog_timeout_ms = 30'000;

device const *const watchdog = DEVICE_DT_GET(DT_NODELABEL(wdt0));
int watchdog_channel = -1;

// Resets the device when the loop below does not come back to feed it, e.g. a controller mutex held forever. A failure is
// only logged: the device works without the watchdog.
auto watchdog_start() -> int {
  if(!device_is_ready(watchdog)) {
    return -ENODEV;
  }
  wdt_timeout_cfg timeout{.window = {.min = 0, .max = watchdog_timeout_ms}, .callback = nullptr, .flags = WDT_FLAG_RESET_SOC};
  watchdog_channel = wdt_install_timeout(watchdog, &timeout);
  if(watchdog_channel < 0) {
    return watchdog_channel;
  }
  return wdt_setup(watchdog, WDT_OPT_PAUSE_HALTED_BY_DBG);
}

adc_dt_spec const vbat = ADC_DT_SPEC_GET_BY_NAME(DT_NODELABEL(adc), vbat);
gpio_dt_spec const stat1 = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), charger_stat1_gpios);
gpio_dt_spec const stat2 = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), charger_stat2_gpios);

// Called by main (boot check) and the battery thread; the first successful setup is the only one, so no channel is
// reconfigured while the other thread reads.
K_MUTEX_DEFINE(setup_mutex);
bool setup_done;

auto setup_once() -> int {
  if(!adc_is_ready_dt(&vbat) || !gpio_is_ready_dt(&stat1) || !gpio_is_ready_dt(&stat2)) {
    return -ENODEV;
  }
  if(int err = adc_channel_setup_dt(&vbat); err != 0) {
    return err;
  }
  if(int err = gpio_pin_configure_dt(&stat1, GPIO_INPUT); err != 0) {
    return err;
  }
  return gpio_pin_configure_dt(&stat2, GPIO_INPUT);
}

auto setup() -> int {
  k_mutex_lock(&setup_mutex, K_FOREVER);
  int err = setup_done ? 0 : setup_once();
  setup_done = err == 0;
  k_mutex_unlock(&setup_mutex);
  return err;
}

auto read_millivolts() -> int {
  std::int16_t sample{};
  adc_sequence sequence{.buffer = &sample, .buffer_size = sizeof(sample)};
  if(int err = adc_sequence_init_dt(&vbat, &sequence); err != 0) {
    return err;
  }
  if(int err = adc_read_dt(&vbat, &sequence); err != 0) {
    return err;
  }
  std::int32_t millivolts = sample;
  if(int err = adc_raw_to_millivolts_dt(&vbat, &millivolts); err != 0) {
    return err;
  }
  return millivolts * vbat_divider;
}

gtfog::battery_status make_status(int millivolts) {
  return {millivolts, gtfog::decode_charger_state(gpio_pin_get_dt(&stat1) > 0, gpio_pin_get_dt(&stat2) > 0)};
}

void battery_thread(void *, void *, void *) {
  if(int err = watchdog_start(); err != 0) {
    LOG_ERR("watchdog: %d", err);
    watchdog_channel = -1;
  }
  // Without a working ADC the loop still counts the idle time for the auto-off.
  int setup_err = setup();
  if(setup_err != 0) {
    LOG_ERR("setup: %d", setup_err);
    // ponytail: the low-battery cutoff and the BAS value are gone until a retry works; show it after the power-on signal.
    k_sleep(fault_signal_delay);
    with_controller([](gtfog::controller &control) { control.report(gtfog::signal::fault); });
  }
  read_failure_guard read_guard;
  std::int64_t counted_ms = k_uptime_get();
  while(true) {
    // The loop runs a little slower than its period; count the real time, keeping the remainder.
    auto idle = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::milliseconds{k_uptime_get() - counted_ms});
    counted_ms += std::chrono::duration_cast<std::chrono::milliseconds>(idle).count();
    if(setup_err != 0) {
      setup_err = setup(); // Retried every period; a failure counts as a failed read below.
    }
    int millivolts = setup_err == 0 ? read_millivolts() : setup_err;
    // Fed right after the controller mutex section, before anything that may block (notifications).
    auto feed_watchdog = [] {
      if(watchdog_channel >= 0) {
        std::ignore = wdt_feed(watchdog, watchdog_channel); // A failed feed ends in a reset, which is the safe outcome.
      }
    };
    if(millivolts < 0) {
      LOG_WRN("ADC read: %d", millivolts);
      // Failing reads, or a setup that keeps failing, mean no cutoff, so they end in a power off.
      auto verdict = read_guard.update(false);
      with_controller([&](gtfog::controller &control) {
        control.elapsed(idle, ble_connected());
        if(verdict == read_failure_guard::verdict::fault) {
          control.report(gtfog::signal::fault);
        } else if(verdict == read_failure_guard::verdict::power_off) {
          control.battery_unreadable(); // Does not return on the board.
        }
      });
      feed_watchdog();
    } else {
      std::ignore = read_guard.update(true);
      auto status = make_status(millivolts);
      LOG_DBG("%d mV, charger %d", millivolts, static_cast<int>(status.charger));
      auto percent = with_controller([&](gtfog::controller &control) {
        control.handle(status);
        control.elapsed(idle, ble_connected());
        return control.battery_level();
      });
      feed_watchdog();
      ble_battery_changed(percent);
      ble_charger_changed(status.charger);
    }
    k_sleep(K_SECONDS(sample_interval_s));
  }
}

K_THREAD_DEFINE(battery, 2048, battery_thread, nullptr, nullptr, nullptr, 7, 0, 500);

} // namespace

auto gtfog::battery_sample() -> std::optional<gtfog::battery_status> {
  if(setup() != 0) {
    return std::nullopt;
  }
  int millivolts = read_millivolts();
  return millivolts < 0 ? std::nullopt : std::optional{make_status(millivolts)};
}
