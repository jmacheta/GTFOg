#ifndef SYSTEM_HPP_
#define SYSTEM_HPP_

#include <expected>
#include "error_codes.hpp"

#include <chrono>

struct uptime_clock {
    using rep        = std::chrono::milliseconds::rep;
    using period     = std::chrono::milliseconds::period;
    using duration   = std::chrono::milliseconds;
    using time_point = std::chrono::time_point<uptime_clock>;

    static constexpr bool is_steady = true;

    static auto now() noexcept -> time_point;
};


template<typename T> void system_process_event(T event);

[[noreturn]] void system_power_off() noexcept;

[[noreturn]] void system_on_unrecoverable_error() noexcept;


auto enable_wake_from_buttons() noexcept -> std::expected<void, error_code>;


#endif