#ifndef BUTTONS_HPP_
#define BUTTONS_HPP_

#include "error_codes.hpp"

#include <expected>

#include <chrono>

enum class button_press_kind {
    short_press,
    long_press,
    stuck,
};


namespace events {

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
} // namespace events

auto enable_wake_from_buttons() noexcept -> std::expected<void, error_code>;

#endif