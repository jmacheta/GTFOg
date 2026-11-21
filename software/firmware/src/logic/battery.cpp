#include "battery.hpp"

#include <array>
#include <utility>

namespace gtfog {

auto decode_charger_state(bool stat1, bool stat2) noexcept -> charger_state {
  // ponytail: the table the firmware used so far; verify against the BQ25185 datasheet on a board.
  if(stat1 && stat2) {
    return charger_state::charge_completed;
  }
  if(stat1) {
    return charger_state::charging;
  }
  if(stat2) {
    return charger_state::fault;
  }
  return charger_state::not_charging;
}

auto battery_percentage(int millivolts) noexcept -> std::uint8_t {
  // ponytail: generic Li-ion curve at light load; measure the real cell to calibrate.
  constexpr std::array<std::pair<int, int>, 8> curve{{
      {3300, 0},
      {3500, 5},
      {3600, 10},
      {3700, 30},
      {3800, 50},
      {3900, 65},
      {4000, 80},
      {4200, 100},
  }};

  if(millivolts <= curve.front().first) {
    return 0;
  }
  for(std::size_t i = 1; i != curve.size(); ++i) {
    auto [v1, p1] = curve[i];
    if(millivolts < v1) {
      auto [v0, p0] = curve[i - 1];
      return static_cast<std::uint8_t>(p0 + (millivolts - v0) * (p1 - p0) / (v1 - v0));
    }
  }
  return 100;
}

auto battery_guard::update(battery_status status) noexcept -> bool {
  bool on_external_power = status.charger == charger_state::charging || status.charger == charger_state::charge_completed;
  if(on_external_power || status.millivolts >= cutoff_millivolts) {
    low_samples = 0;
    return false;
  }
  if(low_samples < samples_below_cutoff) {
    ++low_samples;
  }
  return low_samples == samples_below_cutoff;
}

} // namespace gtfog
