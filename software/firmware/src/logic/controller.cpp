#include "controller.hpp"

#include <algorithm>

namespace gtfog {

auto encode(device_settings s) noexcept -> std::array<std::uint8_t, settings_size> {
  return {static_cast<std::uint8_t>((s.stealth ? 1U : 0U) | (s.transport_lock ? 2U : 0U)),
          s.auto_off_minutes,
          s.fan_step,
          static_cast<std::uint8_t>(s.mode),
          s.motion_delay_s,
          s.motion_speed[0],
          s.motion_speed[1],
          s.motion_speed[2]};
}

auto decode_settings(std::span<std::uint8_t const> bytes) noexcept -> std::optional<device_settings> {
  if(!is_settings_size(bytes.size()) || (bytes[0] & ~3U) != 0) {
    return std::nullopt;
  }
  device_settings s{.stealth = (bytes[0] & 1U) != 0, .transport_lock = (bytes[0] & 2U) != 0, .auto_off_minutes = bytes[1]};
  if(bytes.size() != settings_size_v1) {
    if(bytes[2] < 1 || bytes[2] > 25 || bytes[3] > 1 || bytes[4] < 5) {
      return std::nullopt;
    }
    s.fan_step = bytes[2];
    s.mode = static_cast<fan_mode>(bytes[3]);
    s.motion_delay_s = bytes[4];
    for(std::size_t i = 5; i < bytes.size(); ++i) {
      if(bytes[i] > 100) {
        return std::nullopt;
      }
      s.motion_speed[i - 5] = bytes[i];
    }
  }
  return s;
}

void controller::indicate(signal s) {
  if(!config.stealth) {
    out.show(s);
  }
}

void controller::power_on() {
  indicate(signal::power_on);
  fan = 0;
  level = activity_level::still;
  apply_fan();
  set_strobe(false);
}

void controller::check_start(battery_status status) {
  if(!battery_allows_start(status)) {
    power_off(signal::low_battery);
  }
}

void controller::sensor_lost() {
  level = activity_level::still;
  rising_s = falling_s = 0;
  apply_fan(); // Also tells the phone about the level.
  indicate(signal::fault);
}

void controller::handle(button_event event) {
  idle_seconds = 0;
  if(config.transport_lock && event.action != button_action::power_off && event.action != button_action::unlock) {
    indicate(signal::locked);
    return;
  }
  switch(event.action) {
  case button_action::plus:
    change_fan_speed(static_cast<int>(event.steps * config.fan_step));
    break;
  case button_action::minus:
    change_fan_speed(-static_cast<int>(event.steps * config.fan_step));
    break;
  case button_action::toggle_strobe:
    set_strobe(!strobe_on);
    break;
  case button_action::power_off:
    power_off(signal::power_off);
    break;
  case button_action::unlock:
    if(config.transport_lock) {
      config.transport_lock = false;
      apply_fan();
      apply_strobe();
      out.settings_changed(config);
      indicate(signal::unlocked);
    } else {
      change_fan_speed(config.fan_step);
    }
    break;
  }
}

void controller::change_fan_speed(int delta) {
  int wanted = static_cast<int>(target_fan()) + delta;
  set_fan_speed(static_cast<unsigned>(std::clamp(wanted, 0, 100)));
  bool at_limit = wanted <= 0 || wanted >= 100;
  indicate(at_limit ? signal::speed_limit : (delta > 0 ? signal::speed_up : signal::speed_down));
}

void controller::set_fan_speed(unsigned percent) {
  percent = std::min(percent, 100u);
  if(config.mode == fan_mode::motion) {
    config.motion_speed[static_cast<unsigned>(level)] = static_cast<std::uint8_t>(percent);
    out.settings_changed(config);
  } else {
    fan = percent;
  }
  apply_fan();
}

void controller::update_settings(device_settings written) {
  if(written.mode != config.mode) {
    level = activity_level::still;
    rising_s = falling_s = 0;
  }
  config = written;
  apply_fan();
  apply_strobe(); // Turning the lock on stops them.
  out.settings_changed(config);
}

void controller::observe(activity_level raw) {
  if(config.mode != fan_mode::motion) {
    return;
  }
  auto before = level;
  if(raw > level) {
    rising_to = rising_s++ == 0 ? raw : std::min(rising_to, raw);
    falling_s = 0;
    if(rising_s >= 2) { // ponytail: 2 s up, config.motion_delay_s down; tune with the thresholds.
      level = rising_to;
      rising_s = 0;
    }
  } else {
    rising_s = 0;
    falling_s = raw < level ? falling_s + 1 : 0;
    if(falling_s >= config.motion_delay_s) {
      level = raw;
      falling_s = 0;
    }
  }
  if(level != before) {
    apply_fan(); // Also tells the phone about the new level.
  }
}

void controller::set_strobe(bool on) {
  strobe_on = on;
  apply_strobe();
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
  // In motion mode the fan always runs at some speed, so only movement counts as use.
  bool fan_in_use = config.mode == fan_mode::motion ? level != activity_level::still && !config.transport_lock : fan_speed() != 0;
  if(fan_in_use || strobe() || phone_connected) {
    idle_seconds = 0;
    return;
  }
  idle_seconds += seconds;
  if(config.auto_off_minutes != 0 && idle_seconds >= config.auto_off_minutes * 60U) {
    power_off(signal::power_off);
  }
}

void controller::power_off(signal s) {
  set_strobe(false);
  fan = 0;
  out.set_fan(0);
  indicate(s);
  out.power_off();
}

} // namespace gtfog
