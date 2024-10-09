#include "buttons.hpp"

#include "compile_time_config.hpp"
#include "error_codes.hpp"
#include "system.hpp"

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#include <chrono>
#include <utility>


#define ZEPHYR_USER_NODE DT_PATH(zephyr_user)

static gpio_dt_spec const button_plus  = GPIO_DT_SPEC_GET(DT_NODELABEL(button_plus), gpios);
static gpio_dt_spec const button_minus = GPIO_DT_SPEC_GET(DT_NODELABEL(button_minus), gpios);

static gpio_callback button_plus_on_change_callback;
static gpio_callback button_minus_on_change_callback;

K_SEM_DEFINE(button_changed, 0, 1);


static void on_button_change(device const* dev, gpio_callback* cb, uint32_t pins) {
    k_sem_give(&button_changed);
}


static int buttons_init(void) {
    [[unlikely]] if (!gpio_is_ready_dt(&button_plus) || !gpio_is_ready_dt(&button_minus)) { return -ENOENT; }

    int button_plus_result  = gpio_pin_configure_dt(&button_plus, GPIO_INPUT);
    int button_minus_result = gpio_pin_configure_dt(&button_minus, GPIO_INPUT);


    [[unlikely]] if ((button_plus_result < 0) || (button_minus_result < 0)) { return -EINVAL; }

    button_plus_result  = gpio_pin_interrupt_configure_dt(&button_plus, GPIO_INT_EDGE_BOTH);
    button_minus_result = gpio_pin_interrupt_configure_dt(&button_minus, GPIO_INT_EDGE_BOTH);

    [[unlikely]] if ((button_plus_result < 0) || (button_minus_result < 0)) { return -ENOTSUP; }

    gpio_init_callback(&button_plus_on_change_callback, on_button_change, BIT(button_plus.pin));
    gpio_init_callback(&button_minus_on_change_callback, on_button_change, BIT(button_minus.pin));


    gpio_add_callback(button_plus.port, &button_plus_on_change_callback);
    gpio_add_callback(button_minus.port, &button_minus_on_change_callback);
    return 0;
}


enum class button_state : unsigned {
    released      = 0b0000,
    pressed       = 0b0001,
    short_pressed = 0b0010,
    long_pressed  = 0b1001,
    stuck         = 0b1111,
};


static auto operator|=(button_state& lhs, button_state const& rhs) noexcept -> button_state& {
    lhs = static_cast<button_state>(static_cast<unsigned>(lhs) | static_cast<unsigned>(rhs));
    return lhs;
}

static auto operator&(button_state lhs, button_state rhs) noexcept -> unsigned {
    return static_cast<unsigned>(lhs) & static_cast<unsigned>(rhs);
}


namespace config {
    static_assert(buttons_long_press < buttons_stuck);
    static_assert(buttons_short_press < buttons_long_press);
    static_assert(buttons_short_press < buttons_thread_report_interval);
    static_assert(buttons_short_press >= 10ms);
} // namespace config


auto get_button_state(bool is_pressed, button_state previous, uptime_clock::time_point last_press, uptime_clock::time_point now) {
    auto press_duration = now - last_press;
    bool was_pressed    = (previous & button_state::pressed) != 0;

    if (is_pressed) {
        if (was_pressed) {
            if (press_duration >= config::buttons_stuck) {
                return button_state::stuck;
            }
            if (press_duration >= config::buttons_long_press) {
                return button_state::long_pressed;
            }
        }
        return button_state::pressed;

    } else if (was_pressed) {
        if (press_duration >= config::buttons_short_press && press_duration < config::buttons_long_press) {
            return button_state::short_pressed;
        }
    }

    return button_state::released;
}

static void buttons_thread(void*, void*, void*) {
    using namespace events;
    auto last_plus_press  = uptime_clock::time_point{};
    auto last_minus_press = uptime_clock::time_point{};

    auto plus_previous_state  = button_state::released;
    auto minus_previous_state = button_state::released;

    while (1) {
        auto now = uptime_clock::now();

        bool plus_was_pressed  = (plus_previous_state & button_state::pressed) != 0;
        bool minus_was_pressed = (minus_previous_state & button_state::pressed) != 0;

        bool plus_is_pressed  = gpio_pin_get(button_plus.port, button_plus.pin);
        bool minus_is_pressed = gpio_pin_get(button_minus.port, button_minus.pin);

        auto plus_state = get_button_state(plus_is_pressed, plus_previous_state, last_plus_press, now);
        if (!plus_was_pressed && plus_is_pressed) {
            last_plus_press = now;
        }

        auto is_reportable = [](button_state state, button_state previous) {
            bool is_press_event = (button_state::released != state) && (button_state::pressed != state);
            bool is_repeatable  = (button_state::long_pressed == state);
            bool is_repeated    = (previous == state);
            return is_press_event && (!is_repeated || is_repeatable);
        };
        bool report_plus_press = is_reportable(plus_state, plus_previous_state);
        plus_previous_state    = plus_state;


        auto minus_state = get_button_state(minus_is_pressed, minus_previous_state, last_minus_press, now);
        if (!minus_was_pressed && minus_is_pressed) {
            last_minus_press = now;
        }
        bool report_minus_press = is_reportable(minus_state, minus_previous_state);
        minus_previous_state    = minus_state;


        if (report_plus_press || report_minus_press) {
            auto state_to_kind = [](button_state state) {
                switch (state) {
                    case button_state::short_pressed: return button_press_kind::short_press;
                    case button_state::long_pressed: return button_press_kind::long_press;
                    case button_state::stuck: return button_press_kind::stuck;
                    default: std::unreachable();
                }
            };

            if (report_plus_press && report_minus_press) {
                system_process_event(both_buttons_pressed{
                    .plus  = {.kind = state_to_kind(plus_state), .press_duration = now - last_plus_press},
                    .minus = {.kind = state_to_kind(minus_state), .press_duration = now - last_minus_press},
                });
            } else if (report_plus_press) {
                system_process_event(plus_button_pressed{
                    .kind           = state_to_kind(plus_state),
                    .press_duration = now - last_plus_press,
                });
            } else {
                system_process_event(minus_button_pressed{
                    .kind           = state_to_kind(minus_state),
                    .press_duration = now - last_minus_press,
                });
            }
        }


        bool no_activity = (button_state::released == plus_state) && (button_state::released == minus_state);
        auto next_update = (no_activity) ? K_FOREVER : K_MSEC(config::buttons_thread_report_interval.count());
        k_sem_take(&button_changed, next_update);
    }
    std::unreachable();
}

SYS_INIT_NAMED(buttons, buttons_init, POST_KERNEL, BUTTONS_INITIALIZATION_PRIORITY);

K_THREAD_DEFINE(buttons, config::buttons_thread_stack_size, buttons_thread, nullptr, nullptr, nullptr, config::buttons_thread_priority, 0, 0);


auto enable_wake_from_buttons() noexcept -> std::expected<void, error_code> {
    int button_plus_result  = gpio_remove_callback(button_plus.port, &button_plus_on_change_callback);
    int button_minus_result = gpio_remove_callback(button_minus.port, &button_minus_on_change_callback);

    if ((button_plus_result < 0) || (button_minus_result < 0)) {
        return std::unexpected(error_code{-ENOSYS});
    }
    button_plus_result  = gpio_pin_interrupt_configure_dt(&button_plus, GPIO_INT_LEVEL_ACTIVE);
    button_minus_result = gpio_pin_interrupt_configure_dt(&button_minus, GPIO_INT_LEVEL_ACTIVE);

    if ((button_plus_result < 0) || (button_minus_result < 0)) {
        return std::unexpected(error_code{-ENOTSUP});
    }
    return {};
}
