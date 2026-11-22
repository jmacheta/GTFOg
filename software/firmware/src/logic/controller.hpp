#ifndef GTFOG_LOGIC_CONTROLLER_HPP
#define GTFOG_LOGIC_CONTROLLER_HPP

#include "battery.hpp"
#include "buttons.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>

namespace gtfog {

// What the user sees on the status LED; the board layer picks the colors.
enum class signal {
  power_on,
  speed_up,
  speed_down,
  speed_limit,
  low_battery,
  locked,    // A button did nothing because of the transport lock.
  power_off, // Last, right before power_off(); the board waits for it to finish.
};

// User settings, kept in flash and written over Bluetooth.
struct device_settings {
  bool stealth{false};              // The status LED never lights up.
  bool transport_lock{false};       // Buttons only wake the device and power it off; fan and strobe stay off.
  std::uint8_t auto_off_minutes{0}; // Power off after this long idle (fan and strobe off, no phone); 0 never.

  friend auto operator==(device_settings const &, device_settings const &) -> bool = default;
};

// Wire and flash format: flags (bit 0 stealth, bit 1 transport lock), auto-off minutes.
constexpr std::size_t settings_size = 2;
[[nodiscard]] auto encode(device_settings s) noexcept -> std::array<std::uint8_t, settings_size>;
[[nodiscard]] auto decode_settings(std::span<std::uint8_t const> bytes) noexcept -> std::optional<device_settings>;

// The hardware the controller drives. The firmware implements it on the board, the tests with a fake.
class outputs {
public:
  virtual void set_fan(unsigned percent) = 0; // 0 stops the fan, 1..100 runs it.
  virtual void set_strobe(bool on) = 0;
  virtual void show(signal s) = 0;
  virtual void power_off() = 0; // Does not return on the board.

protected:
  ~outputs() = default;
};

// Maps a fan speed of 0..100 % to a PWM duty cycle of 0..1000 permille. Any speed above 0 gets at least `min_duty`
// so the fan does not stall.
[[nodiscard]] constexpr auto fan_duty_permille(unsigned percent, unsigned min_duty) noexcept -> unsigned {
  if(percent == 0) {
    return 0;
  }
  if(percent > 100) {
    percent = 100;
  }
  return min_duty + (1000 - min_duty) * (percent - 1) / 99;
}

// The device behavior: buttons, Bluetooth requests and battery readings in, fan, strobe and LED out.
class controller {
public:
  explicit controller(outputs &out) noexcept : out(out) {}

  void power_on();
  void handle(button_event event);
  void set_fan_speed(unsigned percent);
  void set_strobe(bool on);
  void handle(battery_status status);
  void configure(device_settings s) noexcept { cfg = s; }
  // Counts idle time for the auto-off; call it periodically with the time since the last call.
  void elapsed(unsigned seconds, bool phone_connected);

  [[nodiscard]] auto fan_speed() const noexcept -> unsigned { return fan; }
  [[nodiscard]] auto strobe() const noexcept -> bool { return strobe_on; }
  [[nodiscard]] auto battery_level() const noexcept -> std::uint8_t { return battery; }
  [[nodiscard]] auto settings() const noexcept -> device_settings { return cfg; }

private:
  void change_fan_speed(int delta);
  void indicate(signal s);
  void power_off(signal s);

  outputs &out;
  unsigned fan{0};
  bool strobe_on{false};
  std::uint8_t battery{0};
  device_settings cfg;
  unsigned idle_seconds{0};
  battery_guard guard;
};

} // namespace gtfog

#endif
