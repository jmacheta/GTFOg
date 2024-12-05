#ifndef EVENTS_HPP_
#define EVENTS_HPP_
#include <chrono>

namespace events {

    using namespace std::chrono_literals;

    struct battery_circuit_failure {
        bool primary_battery_failure;
        bool secondary_battery_failure;
        bool charger_failure;
    };

    struct battery_charge_update {
        unsigned remaining_charge_percentage;
    };

    enum class button_press_kind {
        short_press,
        long_press,
        stuck,
    };

    struct minus_button_pressed {
        button_press_kind         kind;
        std::chrono::milliseconds press_duration;
    };

    struct plus_button_pressed {
        button_press_kind         kind;
        std::chrono::milliseconds press_duration;
    };

    struct both_buttons_pressed {
        plus_button_pressed  plus;
        minus_button_pressed minus;
    };

    struct request_power_on {};

    struct movement_update {
        unsigned movement_level;
    };


} // namespace events

#endif