#include "compile_time_config.hpp"
#include "events.hpp"

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>

static const device* accel = DEVICE_DT_GET(DT_NODELABEL(accelerometer));

static const enum sensor_channel channels[] = {
    SENSOR_CHAN_ACCEL_X,
    SENSOR_CHAN_ACCEL_Y,
    SENSOR_CHAN_ACCEL_Z,
};

static int print_accels(const struct device* dev) {
    int                 ret;
    struct sensor_value accel[3];
    for (int i = 0; i < 3; i++) {
        ret = sensor_sample_fetch(dev);
        if (ret < 0) {
            printk("%s: sensor_sample_fetch() failed: %d\n", dev->name, ret);
            return ret;
        }

        ret = sensor_channel_get(dev, channels[i], &accel[i]);
        if (ret < 0) {
            printk("%s: sensor_channel_get(%c) failed: %d\n", dev->name, 'X' + i, ret);
            return ret;
        }
    }

    printk("%16s [m/s^2]:    (%12.6f, %12.6f, %12.6f)\n", dev->name, sensor_value_to_double(&accel[0]), sensor_value_to_double(&accel[1]), sensor_value_to_double(&accel[2]));

    return 0;
}

static int set_sampling_freq(const struct device* dev) {
    int                 ret;
    struct sensor_value odr;

    ret = sensor_attr_get(dev, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);

    /* If we don't get a frequency > 0, we set one */
    if (ret != 0 || (odr.val1 == 0 && odr.val2 == 0)) {
        odr.val1 = 100;
        odr.val2 = 0;

        ret = sensor_attr_set(dev, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &odr);

        if (ret != 0) {
            printk("%s : failed to set sampling frequency %d\n", dev->name, ret);
        }
    }

    return 0;
}

#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)


static void accelerometer_thread(void*, void*, void*) {
    gpio_dt_spec const pwr = GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, accelerometer_pwr_gpios);
    gpio_pin_configure_dt(&pwr, GPIO_OUTPUT);
    gpio_pin_set_dt(&pwr, 0);
    k_sleep(K_MSEC(100));

    gpio_pin_set_dt(&pwr, 1);
    k_sleep(K_MSEC(500));

    device_init(accel);
    if (!device_is_ready(accel)) {
        printk("sensor: device %s not ready.\n", accel->name);
        return;
    }
    set_sampling_freq(accel);
    while (1) {
        print_accels(accel);
        k_sleep(K_MSEC(1000));
    }
}

K_THREAD_DEFINE(accelerometer, config::accelerometer_thread_stack_size, accelerometer_thread, nullptr, nullptr, nullptr, config::accelerometer_thread_priority, 0, 100);
