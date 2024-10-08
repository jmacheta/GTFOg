
#ifndef SIGNALING_HPP_
#define SIGNALING_HPP_


enum class signaling_state {
    off                              = 0b0000,
    charging                         = 0b0001,
    manual_operation                 = 0b0010,
    bt_connection                    = 0b0100,
    accelerometer_threshold_exceeded = 0b1000,
};



void enable_signaling_of(signaling_state state) noexcept;

void disable_signaling_of(signaling_state state) noexcept;

#endif