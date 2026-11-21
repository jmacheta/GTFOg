#ifndef GTFOG_LOGIC_BATTERY_HPP
#define GTFOG_LOGIC_BATTERY_HPP

#include <cstdint>

namespace gtfog {

// Charger state, from the levels of the BQ25185 STAT1 and STAT2 pins (true = high).
enum class charger_state {
  not_charging,
  charging,
  charge_completed,
  fault,
};

[[nodiscard]] auto decode_charger_state(bool stat1, bool stat2) noexcept -> charger_state;

// Remaining charge of a single Li-ion cell, 0..100 %, from its open-circuit-like voltage.
[[nodiscard]] auto battery_percentage(int millivolts) noexcept -> std::uint8_t;

struct battery_status {
  int millivolts;
  charger_state charger;
};

// Decides when the battery is too low to keep running. A few low samples in a row are needed, so a voltage dip under
// fan inrush does not switch the device off.
class battery_guard {
public:
  static constexpr int cutoff_millivolts = 3300;
  static constexpr unsigned samples_below_cutoff = 3;

  // Returns true once the device should power off.
  [[nodiscard]] auto update(battery_status status) noexcept -> bool;

private:
  unsigned low_samples{0};
};

} // namespace gtfog

#endif
