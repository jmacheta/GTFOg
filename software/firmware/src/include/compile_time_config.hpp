#ifndef COMPILE_TIME_CONFIG_HPP_
#define COMPILE_TIME_CONFIG_HPP_


#define NVS_INITIALIZATION_PRIORITY 20
namespace config {
    constexpr auto accelerometer_thread_priority = 10;
    constexpr auto accelerometer_thread_stack_size = 2048;



}


#endif