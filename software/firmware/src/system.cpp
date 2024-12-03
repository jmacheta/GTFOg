#include "system.hpp"

// #include "accelerometer.hpp"
#include "ble.hpp"
// #include "buttons.hpp"
#include "compile_time_config.hpp"
#include "fan.hpp"
// #include "include/charger_status.hpp"
#include "status_led.hpp"

#include <boost/sml.hpp>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/sys/reboot.h>

#include <algorithm>
#include <tuple>
#include <utility>


auto uptime_clock::now() noexcept -> time_point {
    return time_point{duration{k_uptime_get()}};
}


void system_power_off() noexcept {
    k_sched_lock();

    std::ignore      = irq_lock();
    auto& status_led = status_light_instance();
    auto& fan        = fan_instance();

    std::ignore = status_led.set_color(Colors::White);
    std::ignore = fan.set_speed(0);

    for (volatile int i = 0; i != 10000000; ++i) {
        // do nothing
    }
    std::ignore = status_led.set_color(Colors::Black);

    if (!enable_wake_from_buttons()) {
        system_on_unrecoverable_error();
    }
    sys_poweroff();

    std::unreachable();
}


void system_on_unrecoverable_error() noexcept {
    printk("Unrecoverable error occurred; system will restart\n");
    sys_reboot(SYS_REBOOT_WARM);
    std::unreachable();
}


namespace sml = boost::sml;
using namespace events;
struct my_logger {
    template<class SM, class TEvent> void log_process_event(const TEvent&) {
        // printk("[%s][process_event] %s\n", sml::aux::get_type_name<SM>(), sml::aux::get_type_name<TEvent>());
    }

    template<class SM, class TGuard, class TEvent> void log_guard(const TGuard&, const TEvent&, bool result) {
        // printk("[%s][guard] %s %s %s\n", sml::aux::get_type_name<SM>(), sml::aux::get_type_name<TGuard>(), sml::aux::get_type_name<TEvent>(), (result ? "[OK]" : "[Reject]"));
    }

    template<class SM, class TAction, class TEvent> void log_action(const TAction&, const TEvent&) {
        // printk("[%s][action] %s %s\n", sml::aux::get_type_name<SM>(), sml::aux::get_type_name<TAction>(), sml::aux::get_type_name<TEvent>());
    }

    template<class SM, class TSrcState, class TDstState> void log_state_change(const TSrcState& src, const TDstState& dst) {
        // printk("[%s][transition] %s -> %s\n", sml::aux::get_type_name<SM>(), src.c_str(), dst.c_str());
    }
};


static auto hold_long_enough_for_power_off = [](both_buttons_pressed const& event) {
    bool right_press_kind     = (button_press_kind::long_press == event.plus.kind) && (button_press_kind::long_press == event.minus.kind);
    bool right_press_duration = (event.plus.press_duration >= config::both_buttons_power_off) && (event.minus.press_duration >= config::both_buttons_power_off);

    return right_press_kind && right_press_duration;
};


auto        wtf                = 0;
static auto increase_fan_speed = [](plus_button_pressed const& event) {
    unsigned change_rate = (event.press_duration / config::button_change_rate_threshold) + 1;

    change_rate = std::min(change_rate, config::button_max_change_rate);

    auto& fan = fan_instance();
    wtf += change_rate;

    if (wtf < 100) {
        printk("Increasing fan speed to %d\n", wtf);
        std::ignore = fan.set_speed(wtf);
    } else {
        wtf         = 100;
        std::ignore = fan.set_speed(wtf);

        printk("Fan is already at maximum speed\n");
    }
};


static auto decrease_fan_speed = []() {
    auto& fan = fan_instance();

    wtf--;
    if (wtf > 0) {
        printk("Decreasing fan speed to %d\n", wtf);
        std::ignore = fan.set_speed(wtf);
    } else {
        wtf         = 0;
        std::ignore = fan.set_speed(wtf);


        printk("Fan is already off\n");
    }
};

static auto do_power_on = []() {
    auto& status_led = status_light_instance();
    auto& fan        = fan_instance();

    std::ignore = status_led.set_color(Colors::Green);
    std::ignore = fan.set_speed(0);

    printk("System powered on\n");
};

static auto do_power_off = []() { system_power_off(); };


static auto update_signaling_scheme = [](charger_status_changed const& event) { printk("Charger status changed\n"); };


struct system_state {
    auto operator()() const {
        using namespace sml;
        /**
         * Initial state: *initial_state
         * Transition DSL: src_state + event [ guard ] / action = dst_state
         */
        return make_transition_table(
            *"off"_s + event<power_on> / do_power_on = "on"_s,          //
            "on"_s                                   = "manual_mode"_s, //

            "manual_mode"_s + event<plus_button_pressed> / increase_fan_speed,  //
            "manual_mode"_s + event<minus_button_pressed> / decrease_fan_speed, //
            // "manual_mode"_s + event<minus_button_pressed> [hold_long_enough_for_power_off)] / do_power_off = X, //

            state<_> + event<charger_status_changed> / update_signaling_scheme,
            state<_> + event<both_buttons_pressed>[hold_long_enough_for_power_off] / []() { system_power_off(); } = X //
        );
    }
};

my_logger                                     logger;
sml::sm<system_state, sml::logger<my_logger>> sm{
    logger,
};


K_MUTEX_DEFINE(fsm_mutex);

template<typename T> void system_process_event(T event) {
    k_mutex_lock(&fsm_mutex, K_FOREVER);
    sm.process_event(event);
    k_mutex_unlock(&fsm_mutex);
}

template void system_process_event(events::power_on);
template void system_process_event(events::charger_status_changed);
template void system_process_event(events::plus_button_pressed);
template void system_process_event(events::minus_button_pressed);
template void system_process_event(events::both_buttons_pressed);
