#ifndef GTFOG_LOGIC_CONTROLLER_HPP
#define GTFOG_LOGIC_CONTROLLER_HPP

#include "battery.hpp"
#include "buttons.hpp"

#include <cstdint>

namespace gtfog {

// What the user sees on the status LED; the board layer picks the colors.
enum class signal {
  power_on,
  speed_up,
  speed_down,
  speed_limit,
  low_battery,
};

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

  [[nodiscard]] auto fan_speed() const noexcept -> unsigned { return fan; }
  [[nodiscard]] auto strobe() const noexcept -> bool { return strobe_on; }
  [[nodiscard]] auto battery_level() const noexcept -> std::uint8_t { return battery; }

private:
  void change_fan_speed(int delta);

  outputs &out;
  unsigned fan{0};
  bool strobe_on{false};
  std::uint8_t battery{0};
  battery_guard guard;
};

} // namespace gtfog

#endif
