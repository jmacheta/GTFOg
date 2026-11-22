#include "controller.hpp"

#include <algorithm>

namespace gtfog {

auto encode(device_settings s) noexcept -> std::array<std::uint8_t, settings_size> {
  return {static_cast<std::uint8_t>((s.stealth ? 1U : 0U) | (s.transport_lock ? 2U : 0U)),
          s.auto_off_minutes,
          s.fan_step,
          static_cast<std::uint8_t>(s.mode),
          s.motion_idle_s,
          s.idle_speed};
}

auto decode_settings(std::span<std::uint8_t const> bytes) noexcept -> std::optional<device_settings> {
  if((bytes.size() != settings_size && bytes.size() != settings_size_v1) || (bytes[0] & ~3U) != 0) {
    return std::nullopt;
  }
  device_settings s{.stealth = (bytes[0] & 1U) != 0, .transport_lock = (bytes[0] & 2U) != 0, .auto_off_minutes = bytes[1]};
  if(bytes.size() == settings_size) {
    if(bytes[2] < 1 || bytes[2] > 25 || bytes[3] > 1 || bytes[4] < 5 || bytes[5] > 100) {
      return std::nullopt;
    }
    s.fan_step = bytes[2];
    s.mode = static_cast<fan_mode>(bytes[3]);
    s.motion_idle_s = bytes[4];
    s.idle_speed = bytes[5];
  }
  return s;
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
  if(cfg.transport_lock && event.action != button_action::power_off && event.action != button_action::unlock) {
    indicate(signal::locked);
    return;
  }
  switch(event.action) {
  case button_action::plus:
    change_fan_speed(static_cast<int>(event.steps * cfg.fan_step));
    break;
  case button_action::minus:
    change_fan_speed(-static_cast<int>(event.steps * cfg.fan_step));
    break;
  case button_action::toggle_strobe:
    set_strobe(!strobe_on);
    break;
  case button_action::power_off:
    power_off(signal::power_off);
    break;
  case button_action::unlock:
    if(cfg.transport_lock) {
      cfg.transport_lock = false;
      out.settings_changed(cfg);
      indicate(signal::unlocked);
    } else {
      change_fan_speed(cfg.fan_step);
    }
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
  apply_fan(true); // The target changed: report it even when the fan keeps its idle speed.
}

void controller::apply_fan(bool force) {
  bool still = cfg.mode == fan_mode::motion && still_ms >= cfg.motion_idle_s * 1000U;
  unsigned effective = still ? std::min<unsigned>(fan, cfg.idle_speed) : fan;
  if(force || applied_fan != effective) {
    applied_fan = effective;
    out.set_fan(effective);
  }
}

void controller::update_settings(device_settings s) {
  cfg = s;
  still_ms = 0;
  apply_fan();
  out.settings_changed(cfg);
}

void controller::motion(bool moving, unsigned elapsed_ms) {
  still_ms = moving ? 0 : still_ms + elapsed_ms;
  apply_fan();
}

void controller::set_strobe(bool on) {
  strobe_on = on;
  out.set_strobe(on);
}

void controller::handle(battery_status status) {
  battery = battery_percentage(status.millivolts);
  if(guard.update(status)) {
    power_off(signal::low_battery);
    return;
  }
  if(battery > 15) {
    battery_warned = false;
  } else if(battery <= 10 && !battery_warned) {
    battery_warned = true;
    indicate(signal::battery_warning);
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
