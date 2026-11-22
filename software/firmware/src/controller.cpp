#include "controller.hpp"

#include <algorithm>

namespace gtfog {

namespace {
constexpr std::chrono::seconds locked_auto_off{std::chrono::minutes{1}};
// ponytail: 2 s up, `motion_delay_s` down (one `track_activity` call per second); tune with the thresholds.
constexpr unsigned rise_seconds = 2;
} // namespace

auto encode(device_settings settings) noexcept -> std::array<std::uint8_t, settings_size> {
  return {static_cast<std::uint8_t>((settings.stealth ? 1U : 0U) | (settings.transport_lock ? 2U : 0U)),
          settings.auto_off_minutes,
          settings.fan_step,
          static_cast<std::uint8_t>(settings.mode),
          settings.motion_delay_s,
          settings.motion_speed[0],
          settings.motion_speed[1],
          settings.motion_speed[2]};
}

auto decode_settings(std::span<std::uint8_t const> bytes, device_settings base) noexcept -> std::optional<device_settings> {
  if(!is_settings_size(bytes.size()) || (bytes[0] & ~3U) != 0) {
    return std::nullopt;
  }
  auto settings = base;
  settings.stealth = (bytes[0] & 1U) != 0;
  settings.transport_lock = (bytes[0] & 2U) != 0;
  settings.auto_off_minutes = bytes[1];
  if(bytes.size() != settings_size_v1) {
    if(bytes[2] < 1 || bytes[2] > 25 || bytes[3] > 1 || bytes[4] < 5) {
      return std::nullopt;
    }
    settings.fan_step = bytes[2];
    settings.mode = static_cast<fan_mode>(bytes[3]);
    settings.motion_delay_s = bytes[4];
    for(std::size_t i = 5; i < bytes.size(); ++i) {
      if(bytes[i] > 100) {
        return std::nullopt;
      }
      settings.motion_speed[i - 5] = bytes[i];
    }
  }
  return settings;
}

void controller_data::indicate(signal shown) {
  if(!config.stealth) {
    out.show(shown);
  }
}

void controller_data::start() {
  indicate(signal::power_on);
  fan = 0;
  level = activity_level::still;
  apply_fan();
  set_strobe(false);
}

void controller_data::lose_sensor() {
  reset_activity();
  apply_fan(); // Also tells the phone about the level.
  indicate(signal::fault);
}

void controller_data::reset_activity() {
  level = activity_level::still; // The accelerometer was off, so the old level says nothing.
  rising_s = falling_s = 0;
}

void controller_data::unlock_by_gesture() {
  config.transport_lock = false;
  reset_activity();
  apply_fan();
  apply_strobe();
  out.settings_changed(config);
  indicate(signal::unlocked);
}

void controller_data::change_fan_speed(int delta) {
  int wanted = static_cast<int>(target_fan()) + delta;
  set_fan_speed(static_cast<unsigned>(std::clamp(wanted, 0, 100)));
  bool at_limit = wanted <= 0 || wanted >= 100;
  indicate(at_limit ? signal::speed_limit : (delta > 0 ? signal::speed_up : signal::speed_down));
}

void controller_data::set_fan_speed(unsigned percent) {
  percent = std::min(percent, 100u);
  if(config.mode == fan_mode::motion) {
    config.motion_speed[static_cast<unsigned>(level)] = static_cast<std::uint8_t>(percent);
    out.settings_changed(config);
  } else {
    fan = percent;
  }
  apply_fan();
}

// The lock and the mode stay: only the entry actions of the states change them.
void controller_data::store_settings(device_settings const &value) {
  auto lock = config.transport_lock;
  auto mode = config.mode;
  config = value;
  config.transport_lock = lock;
  config.mode = mode;
}

void controller_data::publish_settings() {
  apply_fan();
  apply_strobe(); // Turning the lock on stops them.
  out.settings_changed(config);
}

void controller_data::track_activity(activity_level raw) {
  auto before = level;
  if(raw > level) {
    rising_to = rising_s++ == 0 ? raw : std::min(rising_to, raw);
    falling_s = 0;
    if(rising_s >= rise_seconds) {
      level = rising_to;
      rising_s = 0;
    }
  } else {
    rising_s = 0;
    if(raw < level) {
      falling_to = falling_s++ == 0 ? raw : std::max(falling_to, raw);
    } else {
      falling_s = 0;
    }
    if(falling_s >= config.motion_delay_s) {
      level = falling_to; // Mirrors the rise: the highest level of the whole window.
      falling_s = 0;
    }
  }
  if(level != before) {
    apply_fan(); // Also tells the phone about the new level.
  }
}

void controller_data::set_strobe(bool on) {
  strobe_on = on;
  apply_strobe();
}

void controller_data::record_battery(battery_status status) {
  battery = battery_percentage(status.millivolts);
  low_battery_samples = battery <= warning_percent ? low_battery_samples + 1 : 0;
  exhausted = guard.update(status);
}

auto controller_data::in_use(bool phone_connected) const noexcept -> bool {
  // In motion mode the fan always runs at some speed, so only movement counts as use.
  bool fan_in_use = config.mode == fan_mode::motion ? level != activity_level::still && !config.transport_lock : fan_speed() != 0;
  return fan_in_use || strobe() || phone_connected;
}

auto controller_data::idle_limit() const noexcept -> std::chrono::seconds {
  // A locked device that is woken by accident (in a bag) must not idle until the cell is empty.
  return config.transport_lock ? locked_auto_off : std::chrono::minutes{config.auto_off_minutes};
}

void controller_data::shut_down(signal shown) {
  set_strobe(false);
  fan = 0;
  out.set_fan(0);
  indicate(shown);
  out.power_off();
}

} // namespace gtfog
