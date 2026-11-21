#ifndef GTFOG_APP_HPP
#define GTFOG_APP_HPP

#include "controller.hpp"

#include <zephyr/kernel.h>

#include <cstddef>
#include <span>
#include <utility>

extern k_mutex controller_mutex;
[[nodiscard]] auto app_controller() noexcept -> gtfog::controller &;

// Runs `f(controller &)` with the controller locked; every thread goes through here.
template <typename F> decltype(auto) with_controller(F &&f) {
  struct lock {
    lock() { k_mutex_lock(&controller_mutex, K_FOREVER); }
    ~lock() { k_mutex_unlock(&controller_mutex); }
  } held;
  return std::forward<F>(f)(app_controller());
}

// Blocks until both buttons are released, then arms them to wake the device from System OFF.
void buttons_arm_wake();

[[nodiscard]] auto ble_init() -> int;
void ble_state_changed(); // Notifies the fan speed and strobe state to a connected phone.
void ble_battery_changed(std::uint8_t percent);
void ble_charger_changed(gtfog::charger_state state);
[[nodiscard]] auto ble_accel_samples_per_packet() -> std::size_t; // 0 when nobody is connected.
void ble_accel_send(std::span<std::uint8_t const> packet);

void accelerometer_stream(bool on);

#endif
