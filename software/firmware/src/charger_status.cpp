#include "charger_status.hpp"

#include <zephyr/drivers/gpio.h>

#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)

static gpio_dt_spec const stat1 = GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, charger_stat1_gpios);
static gpio_dt_spec const stat2 = GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, charger_stat2_gpios);


auto get_charger_status() noexcept -> std::expected<ChargerStatus, error_code> {
    using enum error_code;
    // gpio_pin_configure_dt(&stat1, GPIO_INPUT);
    // gpio_pin_configure_dt(&stat2, GPIO_INPUT);

    auto s1 = gpio_pin_get(stat1.port, stat1.pin);
    auto s2 = gpio_pin_get(stat2.port, stat2.pin);


    if (s1 < 0 && s2 < 0) {
        return std::unexpected(device_not_ready);
    }

    return ChargerStatus{(s1 << 1) | s2};
}