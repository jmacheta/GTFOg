#ifndef GTFOG_APP_HPP
#define GTFOG_APP_HPP

#include "controller.hpp"
#include "rf_test.hpp"

#include <zephyr/kernel.h>

#include <cstddef>
#include <optional>
#include <span>
#include <utility>

namespace gtfog {

extern k_mutex controller_mutex;
[[nodiscard]] auto app_controller() noexcept -> controller &;

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

// Loads the settings of the "gtfog" subtree from flash into the controller; independent of Bluetooth.
void app_settings_load();
[[nodiscard]] auto ble_init() -> int;
[[nodiscard]] auto ble_connected() -> bool;
void ble_state_changed(); // Notifies the fan speed and strobe state to a connected phone.
void ble_battery_changed(std::uint8_t percent);
void ble_charger_changed(charger_state state);
[[nodiscard]] auto ble_accelerometer_samples_per_packet() -> std::size_t; // 0 when nobody is connected.
void ble_accelerometer_send(std::span<std::uint8_t const> packet);

void ble_settings_changed(device_settings const &settings); // Notifies the phone and stores them in flash after 2 s; does not block.
void ble_settings_flush();                                  // Stores them now, in the calling thread (before System OFF).

// RF carrier test (rf_test.cpp): -EBUSY when one is already starting. While one runs, button gestures and controller outputs are ignored.
[[nodiscard]] auto rf_test_start(rf_test_request const &request) -> int;
[[nodiscard]] auto rf_test_running() -> bool;
void board_rf_test_tick();                    // Fan, strobe and controller signals off for good; blinks the status LED once. Every second.
[[nodiscard]] auto buttons_pressed() -> bool; // Either button is down right now.
[[nodiscard]] auto ble_stop() -> int;         // Disconnects the phone and shuts the Bluetooth stack down; the radio is free afterwards.

// One synchronous battery sample (boot check); empty when the ADC does not work.
[[nodiscard]] auto battery_sample() -> std::optional<battery_status>;

void accelerometer_stream(bool on);
void accelerometer_motion_mode(bool on); // Powers the sensor for motion detection while on.
void accelerometer_power_off();          // Cuts the sensor's power before System OFF; any thread, never takes the controller mutex.

} // namespace gtfog

#endif
