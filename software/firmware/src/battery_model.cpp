#include "battery_model.hpp"

#include <array>
#include <utility>

namespace gtfog {

auto decode_charger_state(bool stat1, bool stat2) noexcept -> charger_state {
  // BQ25185 datasheet, status pin table: high/high is done, sleeping or no input; high/low is charging; low/x is a
  // recoverable (low/high) or latched (low/low) fault.
  if(!stat1) {
    return charger_state::fault;
  }
  return stat2 ? charger_state::not_charging : charger_state::charging;
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
    auto [upper_millivolts, upper_percent] = curve[i];
    if(millivolts < upper_millivolts) {
      auto [lower_millivolts, lower_percent] = curve[i - 1];
      return static_cast<std::uint8_t>(lower_percent +
                                       (millivolts - lower_millivolts) * (upper_percent - lower_percent) / (upper_millivolts - lower_millivolts));
    }
  }
  return 100;
}

auto battery_guard::update(battery_status status) noexcept -> bool {
  bool on_external_power = status.charger == charger_state::charging;
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
