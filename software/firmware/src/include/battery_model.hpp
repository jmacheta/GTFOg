#ifndef GTFOG_BATTERY_MODEL_HPP
#define GTFOG_BATTERY_MODEL_HPP

#include <cstdint>

namespace gtfog {

// Charger state, from the levels of the BQ25185 STAT1 and STAT2 pins (true = high).
// The values go over Bluetooth; 2 is unused, the pins do not tell "charge completed" apart from "no input".
enum class charger_state : std::uint8_t {
  not_charging = 0, // Also charge done and no USB power.
  charging = 1,
  fault = 3,
};

[[nodiscard]] auto decode_charger_state(bool stat1, bool stat2) noexcept -> charger_state;

// Remaining charge of a single Li-ion cell, 0..100 %, from its open-circuit-like voltage.
[[nodiscard]] auto battery_percentage(int millivolts) noexcept -> std::uint8_t;

struct battery_status {
  int millivolts;
  charger_state charger;
};

// At boot the cell must be above the cutoff by some margin (or the charger must run), or the device powers off again at
// once: a rested cell recovers above the cutoff and would otherwise run the fan for a few samples on every button press.
// ponytail: calibrate with the cutoff.
constexpr int start_millivolts = 3400;
[[nodiscard]] constexpr auto battery_allows_start(battery_status status) noexcept -> bool {
  return status.charger == charger_state::charging || status.millivolts >= start_millivolts;
}

// Decides when the battery is too low to keep running. A few low samples in a row are needed, so a voltage dip under
// fan inrush does not switch the device off.
class battery_guard {
public:
  // ponytail: calibrate against the real cell under fan load.
  static constexpr int cutoff_millivolts = 3300;
  static constexpr unsigned samples_below_cutoff = 3;

  // Returns true once the device should power off.
  [[nodiscard]] auto update(battery_status status) noexcept -> bool;

private:
  unsigned low_samples{0};
};

// Watches the voltage reads of the battery thread (one per 10 s): the cutoff above does not work while they fail. A persistent
// failure shows the fault signal once, a longer one powers the device off rather than run it unguarded.
class read_failure_guard {
public:
  // ponytail: 3 failed reads (30 s) show the fault, 6 (60 s) power off; lengthen if a noisy ADC trips it.
  static constexpr unsigned fault_reads = 3;
  static constexpr unsigned power_off_reads = 6;
  enum class verdict { ok, fault, power_off };

  // `read_ok`: the read of this sample succeeded. `fault` and `power_off` are each returned once per run of failures.
  [[nodiscard]] auto update(bool read_ok) noexcept -> verdict {
    if(read_ok) {
      failed = 0;
      return verdict::ok;
    }
    if(failed <= power_off_reads) { // One past the last verdict: nothing more is reported.
      ++failed;
    }
    return failed == power_off_reads ? verdict::power_off : (failed == fault_reads ? verdict::fault : verdict::ok);
  }

private:
  unsigned failed{0};
};

} // namespace gtfog

#endif
