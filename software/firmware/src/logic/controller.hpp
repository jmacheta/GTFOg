#ifndef GTFOG_LOGIC_CONTROLLER_HPP
#define GTFOG_LOGIC_CONTROLLER_HPP

#include "accel.hpp"
#include "battery.hpp"
#include "buttons.hpp"

#include <array>
#include <chrono>
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
  locked,          // A button did nothing because of the transport lock.
  unlocked,        // The unlock gesture cleared the transport lock.
  battery_warning, // The battery is nearly empty; the device keeps running.
  pairing_refused, // A new phone tried to pair after the pairing window closed.
  fault,           // A part (buttons, ADC, accelerometer) failed for good; the device keeps running without it.
  power_off,       // Last, right before power_off(); the board waits for it to finish.
};

enum class fan_mode : std::uint8_t {
  manual = 0, // The fan runs at the speed the user set.
  motion = 1, // The fan speed follows the activity: one target per `activity_level`.
};

// User settings, kept in flash and written over Bluetooth.
struct device_settings {
  bool stealth{false};              // The status LED never lights up.
  bool transport_lock{false};       // Buttons only wake the device and power it off; fan and strobe stay off.
  std::uint8_t auto_off_minutes{0}; // Power off after this long idle (fan and strobe off, no phone); 0 never. While locked: 1 minute.
  std::uint8_t fan_step{5};         // Fan change per button click, in %, 1..25.
  fan_mode mode{fan_mode::manual};
  std::uint8_t motion_delay_s{60};                       // Motion mode: seconds of lower activity before the fan slows down, 5..255.
  std::array<std::uint8_t, 3> motion_speed{20, 50, 100}; // Motion mode: fan speed in % while still, walking, running, 0..100.

  // The accelerometer is only needed while the fan follows it; the transport lock keeps the fan off.
  [[nodiscard]] constexpr auto follows_motion() const noexcept -> bool { return mode == fan_mode::motion && !transport_lock; }

  friend auto operator==(device_settings const &, device_settings const &) -> bool = default;
};

// Wire and flash format: flags (bit 0 stealth, bit 1 transport lock), auto-off minutes, fan step, fan mode, motion delay in
// seconds, still, walking and running speed. Shorter 6- and 2-byte forms are accepted; fields they do not carry keep the
// values of `base`.
constexpr std::size_t settings_size = 8;
constexpr std::size_t settings_size_v2 = 6;
constexpr std::size_t settings_size_v1 = 2;
[[nodiscard]] constexpr auto is_settings_size(std::size_t len) noexcept -> bool {
  return len == settings_size || len == settings_size_v2 || len == settings_size_v1;
}
// Values a phone may write to the fan and strobe characteristics.
[[nodiscard]] constexpr auto is_fan_percent(unsigned value) noexcept -> bool { return value <= 100; }
[[nodiscard]] constexpr auto is_strobe_value(unsigned value) noexcept -> bool { return value <= 1; }
[[nodiscard]] auto encode(device_settings s) noexcept -> std::array<std::uint8_t, settings_size>;
[[nodiscard]] auto decode_settings(std::span<std::uint8_t const> bytes, device_settings base = {}) noexcept -> std::optional<device_settings>;

// The hardware the controller drives. The firmware implements it on the board, the tests with a fake.
class outputs {
public:
  virtual void set_fan(unsigned percent) = 0; // 0 stops the fan, 1..100 runs it.
  virtual void set_strobe(bool on) = 0;
  virtual void show(signal s) = 0;
  virtual void power_off() = 0;                                // Does not return on the board.
  virtual void settings_changed(device_settings const &s) = 0; // After the user changed them; to be stored. Must not block.

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

// Pulse width in ns of a PWM period of `period_ns` at `permille` / 1000 duty; 64-bit, as 5 ms x 1000 overflows 32 bits.
[[nodiscard]] constexpr auto pwm_pulse_ns(std::uint32_t period_ns, unsigned permille) noexcept -> std::uint32_t {
  return static_cast<std::uint32_t>(std::uint64_t{period_ns} * permille / 1000);
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
  // Settings loaded from flash.
  void configure(device_settings loaded) noexcept { config = loaded; }
  // The battery sample taken at boot: powers off at once, without starting the fan, when it is too low to run.
  void check_start(battery_status status);
  // The status LED shows `s` (unless stealth); for the board layer's own events.
  void report(signal s) { indicate(s); }
  // The accelerometer does not answer any more: motion mode runs at the still speed.
  void sensor_lost();
  // Settings written by the user (Bluetooth): stored through `outputs::settings_changed`.
  void update_settings(device_settings written);
  // Activity measured over one second of accelerometer data; only used in motion mode.
  void observe(activity_level raw);
  // Counts idle time for the auto-off; call it periodically with the time since the last call.
  void elapsed(std::chrono::seconds passed, bool phone_connected);

  // What the fan runs at: the manual target, or in motion mode the target of the current activity level; 0 while the
  // transport lock is on (the targets are kept).
  [[nodiscard]] auto fan_speed() const noexcept -> unsigned { return config.transport_lock ? 0 : target_fan(); }
  // The current activity level for Bluetooth: 0 still, 1 walking, 2 running, 255 in manual mode.
  [[nodiscard]] auto activity_code() const noexcept -> std::uint8_t {
    return config.mode == fan_mode::motion ? static_cast<std::uint8_t>(level) : 255;
  }
  [[nodiscard]] auto strobe() const noexcept -> bool { return strobe_on && !config.transport_lock; }
  [[nodiscard]] auto battery_level() const noexcept -> std::uint8_t { return battery; }
  [[nodiscard]] auto settings() const noexcept -> device_settings { return config; }

private:
  void change_fan_speed(int delta);
  [[nodiscard]] auto target_fan() const noexcept -> unsigned {
    return config.mode == fan_mode::motion ? config.motion_speed[static_cast<unsigned>(level)] : fan;
  }
  void apply_fan() { out.set_fan(fan_speed()); }
  void apply_strobe() { out.set_strobe(strobe()); }
  void indicate(signal s);
  void power_off(signal s);

  outputs &out;
  unsigned fan{0}; // The manual mode target.
  activity_level level{activity_level::still};
  activity_level rising_to{activity_level::still}; // Lowest level seen while the raw level stayed above `level`.
  unsigned rising_s{0};                            // Consecutive seconds above `level`.
  unsigned falling_s{0};                           // Consecutive seconds below `level`.
  bool strobe_on{false};
  std::uint8_t battery{0};
  device_settings config;
  std::chrono::seconds idle{0};
  bool battery_warned{false};
  battery_guard guard;
};

} // namespace gtfog

#endif
