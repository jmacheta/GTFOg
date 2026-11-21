// Powers up the ADXL345 and logs its samples. Fan control from movement is not implemented yet.
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(accelerometer, LOG_LEVEL_INF);

namespace {

device const *const accel = DEVICE_DT_GET(DT_NODELABEL(accelerometer));
gpio_dt_spec const power = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), accelerometer_pwr_gpios);

auto power_up() -> int {
  if(int err = gpio_pin_configure_dt(&power, GPIO_OUTPUT_INACTIVE); err != 0) {
    return err;
  }
  k_msleep(100);
  if(int err = gpio_pin_set_dt(&power, 1); err != 0) {
    return err;
  }
  k_msleep(500);
  if(int err = device_init(accel); err != 0) {
    return err;
  }
  sensor_value odr{.val1 = 100, .val2 = 0};
  return sensor_attr_set(accel, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);
}

void accelerometer_thread(void *, void *, void *) {
  if(int err = power_up(); err != 0) {
    LOG_ERR("power up: %d", err);
    return;
  }
  while(true) {
    sensor_value xyz[3];
    if(sensor_sample_fetch(accel) == 0 && sensor_channel_get(accel, SENSOR_CHAN_ACCEL_XYZ, xyz) == 0) {
      LOG_DBG("%d.%06d %d.%06d %d.%06d m/s^2", xyz[0].val1, xyz[0].val2, xyz[1].val1, xyz[1].val2, xyz[2].val1, xyz[2].val2);
    }
    k_sleep(K_SECONDS(1));
  }
}

K_THREAD_DEFINE(accelerometer, 1024, accelerometer_thread, nullptr, nullptr, nullptr, 10, 0, 100);

} // namespace
