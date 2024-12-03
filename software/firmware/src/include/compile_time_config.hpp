#ifndef COMPILE_TIME_CONFIG_HPP_
#define COMPILE_TIME_CONFIG_HPP_

#include <SI/electric_potential.h>

#include <chrono>



#define NVS_INITIALIZATION_PRIORITY            20
#define CHARGER_STATUS_INITIALIZATION_PRIORITY 30
#define BUTTONS_INITIALIZATION_PRIORITY        10

namespace config {
    using namespace std::chrono_literals;
    using namespace SI::literals;


    constexpr auto accelerometer_thread_priority   = 10u;
    constexpr auto accelerometer_thread_stack_size = 2048u;

    constexpr auto buttons_thread_priority        = 5u;
    constexpr auto buttons_thread_stack_size      = 1024u;
    constexpr auto buttons_thread_report_interval = 200ms;
    constexpr auto buttons_short_press            = 50ms;
    constexpr auto buttons_long_press             = 500ms;
    constexpr auto buttons_stuck                  = 30s;


    constexpr auto charger_status_thread_priority   = 5u;
    constexpr auto charger_status_thread_stack_size = 1024u;

    constexpr auto button_change_rate_threshold = 3s;
    constexpr auto button_max_change_rate       = 5u;

    constexpr auto both_buttons_power_off = 3s;


    constexpr SI::milli_volt_t<int64_t> battery_cutoff_voltage = 3300_mV;


} // namespace config


#endif