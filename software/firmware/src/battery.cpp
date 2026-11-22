// Measures the battery and reads the charger status pins.
#include "app.hpp"

#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/logging/log.h>

#include <tuple>

LOG_MODULE_REGISTER(battery, LOG_LEVEL_INF);

namespace {

// ponytail: VBAT reaches AIN4 through a 47k/47k divider (POWER sheet); calibrate against a multimeter.
constexpr int vbat_divider = 2;
constexpr unsigned sample_interval_s = 10; // Also the auto-off resolution.
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

auto setup() -> int {
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

void battery_thread(void *, void *, void *) {
  if(int err = watchdog_start(); err != 0) {
    LOG_ERR("watchdog: %d", err);
    watchdog_channel = -1;
  }
  // Without a working ADC the loop still counts the idle time for the auto-off.
  int setup_err = setup();
  if(setup_err != 0) {
    LOG_ERR("setup: %d", setup_err);
  }
  while(true) {
    int millivolts = setup_err == 0 ? read_millivolts() : setup_err;
    if(millivolts < 0) {
      LOG_WRN("ADC read: %d", millivolts);
      with_controller([](gtfog::controller &c) { c.elapsed(sample_interval_s, ble_connected()); });
    } else {
      gtfog::battery_status status{millivolts, gtfog::decode_charger_state(gpio_pin_get_dt(&stat1) > 0, gpio_pin_get_dt(&stat2) > 0)};
      LOG_DBG("%d mV, charger %d", millivolts, static_cast<int>(status.charger));
      auto percent = with_controller([&](gtfog::controller &c) {
        c.handle(status);
        c.elapsed(sample_interval_s, ble_connected());
        return c.battery_level();
      });
      ble_battery_changed(percent);
      ble_charger_changed(status.charger);
    }
    if(watchdog_channel >= 0) {
      std::ignore = wdt_feed(watchdog, watchdog_channel);
    }
    k_sleep(K_SECONDS(sample_interval_s));
  }
}

K_THREAD_DEFINE(battery, 1024, battery_thread, nullptr, nullptr, nullptr, 7, 0, 500);

} // namespace
