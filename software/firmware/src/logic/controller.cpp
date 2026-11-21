#include "controller.hpp"

#include <algorithm>

namespace gtfog {

auto encode(device_settings s) noexcept -> std::array<std::uint8_t, settings_size> {
  return {static_cast<std::uint8_t>((s.stealth ? 1U : 0U) | (s.transport_lock ? 2U : 0U)), s.auto_off_minutes};
}

auto decode_settings(std::span<std::uint8_t const> bytes) noexcept -> std::optional<device_settings> {
  if(bytes.size() != settings_size || (bytes[0] & ~3U) != 0) {
    return std::nullopt;
  }
  return device_settings{(bytes[0] & 1U) != 0, (bytes[0] & 2U) != 0, bytes[1]};
}

void controller::indicate(signal s) {
  if(!cfg.stealth) {
    out.show(s);
  }
}

void controller::power_on() {
  indicate(signal::power_on);
  set_fan_speed(0);
  set_strobe(false);
}

void controller::handle(button_event event) {
  idle_seconds = 0;
  if(cfg.transport_lock && event.action != button_action::power_off) {
    indicate(signal::locked);
    return;
  }
  switch(event.action) {
  case button_action::plus:
    change_fan_speed(static_cast<int>(event.steps));
    break;
  case button_action::minus:
    change_fan_speed(-static_cast<int>(event.steps));
    break;
  case button_action::toggle_strobe:
    set_strobe(!strobe_on);
    break;
  case button_action::power_off:
    power_off(signal::power_off);
    break;
  }
}

void controller::change_fan_speed(int delta) {
  int wanted = static_cast<int>(fan) + delta;
  set_fan_speed(static_cast<unsigned>(std::clamp(wanted, 0, 100)));
  bool at_limit = wanted <= 0 || wanted >= 100;
  indicate(at_limit ? signal::speed_limit : (delta > 0 ? signal::speed_up : signal::speed_down));
}

void controller::set_fan_speed(unsigned percent) {
  fan = std::min(percent, 100u);
  out.set_fan(fan);
}

void controller::set_strobe(bool on) {
  strobe_on = on;
  out.set_strobe(on);
}

void controller::handle(battery_status status) {
  battery = battery_percentage(status.millivolts);
  if(guard.update(status)) {
    power_off(signal::low_battery);
  }
}

void controller::elapsed(unsigned seconds, bool phone_connected) {
  if(fan != 0 || strobe_on || phone_connected) {
    idle_seconds = 0;
    return;
  }
  idle_seconds += seconds;
  if(cfg.auto_off_minutes != 0 && idle_seconds >= cfg.auto_off_minutes * 60U) {
    power_off(signal::power_off);
  }
}

void controller::power_off(signal s) {
  set_strobe(false);
  set_fan_speed(0);
  indicate(s);
  out.power_off();
}

} // namespace gtfog
