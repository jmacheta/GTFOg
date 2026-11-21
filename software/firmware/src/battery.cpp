// Measures the battery and reads the charger status pins.
#include "app.hpp"

#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(battery, LOG_LEVEL_INF);

namespace {

// ponytail: VBAT reaches AIN4 through a 47k/47k divider (POWER sheet); calibrate against a multimeter.
constexpr int vbat_divider = 2;
constexpr unsigned sample_interval_s = 10; // Also the auto-off resolution.

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
  if(int err = setup(); err != 0) {
    LOG_ERR("setup: %d", err);
    return;
  }
  while(true) {
    int millivolts = read_millivolts();
    if(millivolts < 0) {
      LOG_WRN("ADC read: %d", millivolts);
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
    k_sleep(K_SECONDS(sample_interval_s));
  }
}

K_THREAD_DEFINE(battery, 1024, battery_thread, nullptr, nullptr, nullptr, 7, 0, 500);

} // namespace
