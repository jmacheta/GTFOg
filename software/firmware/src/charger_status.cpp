#include "charger_status.hpp"

#include "compile_time_config.hpp"
#include "error_codes.hpp"
#include "system.hpp"

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <expected>
#include <utility>

#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)

static gpio_dt_spec const stat1 = GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, charger_stat1_gpios);
static gpio_dt_spec const stat2 = GPIO_DT_SPEC_GET(ZEPHYR_USER_NODE, charger_stat2_gpios);

static gpio_callback stat1_on_change_callback;
static gpio_callback stat2_on_change_callback;


K_SEM_DEFINE(charger_status_pins_changed, 0, 1);

static void on_stat_change(device const*, gpio_callback*, uint32_t) {
    k_sem_give(&charger_status_pins_changed);
}


static auto get_charger_status() noexcept -> std::expected<charger_status, error_code> {
    using enum error_code;

    auto s1 = gpio_pin_get(stat1.port, stat1.pin);
    auto s2 = gpio_pin_get(stat2.port, stat2.pin);

    [[unlikely]] if (s1 < 0) { return std::unexpected(error_code{s1}); }
    [[unlikely]] if (s2 < 0) { return std::unexpected(error_code{s2}); }

    return charger_status{(s1 << 1) | s2};
}


static int charger_status_init(void) {
    using namespace events;
    [[unlikely]] if (!gpio_is_ready_dt(&stat1) || !gpio_is_ready_dt(&stat2)) { return -ENOENT; }

    int stat1_result = gpio_pin_configure_dt(&stat1, GPIO_INPUT);
    int stat2_result = gpio_pin_configure_dt(&stat2, GPIO_INPUT);

    [[unlikely]] if ((stat1_result < 0) || (stat2_result < 0)) { return -EINVAL; }

    stat1_result = gpio_pin_interrupt_configure_dt(&stat1, GPIO_INT_EDGE_BOTH);
    stat2_result = gpio_pin_interrupt_configure_dt(&stat2, GPIO_INT_EDGE_BOTH);

    [[unlikely]] if ((stat1_result < 0) || (stat2_result < 0)) { return -ENOTSUP; }


    gpio_init_callback(&stat1_on_change_callback, on_stat_change, BIT(stat1.pin));
    gpio_init_callback(&stat2_on_change_callback, on_stat_change, BIT(stat2.pin));

    gpio_add_callback(stat1.port, &stat1_on_change_callback);
    gpio_add_callback(stat2.port, &stat2_on_change_callback);


    while (1) {
        k_sem_take(&charger_status_pins_changed, K_FOREVER);
        printk("charger status changed\n");
        auto status = get_charger_status().value_or(charger_status::recoverable_fault);
        system_process_event(charger_status_changed{status});
    }
    std::unreachable();
}

K_THREAD_DEFINE(charger_status, config::charger_status_thread_stack_size, charger_status_init, nullptr, nullptr, nullptr, config::charger_status_thread_priority, 0, 0);
