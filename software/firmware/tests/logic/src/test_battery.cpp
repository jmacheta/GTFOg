#include "battery.hpp"

#include <zephyr/ztest.h>

using namespace gtfog;

ZTEST_SUITE(battery, nullptr, nullptr, nullptr, nullptr, nullptr);

ZTEST(battery, test_percentage_is_clamped_and_monotonic) {
  zassert_equal(battery_percentage(2800), 0);
  zassert_equal(battery_percentage(3300), 0);
  zassert_equal(battery_percentage(4200), 100);
  zassert_equal(battery_percentage(4500), 100);

  unsigned previous = 0;
  for(int mv = 3300; mv <= 4200; mv += 10) {
    unsigned p = battery_percentage(mv);
    zassert_true(p >= previous, "%d mV gives %u %% after %u %%", mv, p, previous);
    previous = p;
  }
}

ZTEST(battery, test_percentage_interpolates) {
  zassert_equal(battery_percentage(3750), 40); // Halfway between 3700 mV (30 %) and 3800 mV (50 %).
}

ZTEST(battery, test_charger_states) {
  zassert_equal(decode_charger_state(true, true), charger_state::not_charging);
  zassert_equal(decode_charger_state(true, false), charger_state::charging);
  zassert_equal(decode_charger_state(false, true), charger_state::fault);
  zassert_equal(decode_charger_state(false, false), charger_state::fault);
}

ZTEST(battery, test_guard_needs_consecutive_low_samples) {
  battery_guard guard;
  battery_status low{3200, charger_state::not_charging};
  battery_status ok{3700, charger_state::not_charging};

  zassert_false(guard.update(low));
  zassert_false(guard.update(low));
  zassert_false(guard.update(ok)); // A good sample restarts the count.
  zassert_false(guard.update(low));
  zassert_false(guard.update(low));
  zassert_true(guard.update(low));
}

ZTEST(battery, test_guard_ignores_low_voltage_while_charging) {
  battery_guard guard;
  for(int i = 0; i != 10; ++i) {
    zassert_false(guard.update({3000, charger_state::charging}));
  }
}

ZTEST(battery, test_guard_cutoff_boundary_and_charging_sample_in_between) {
  battery_guard guard;
  battery_status edge{battery_guard::cutoff_millivolts, charger_state::not_charging};
  for(int i = 0; i != 5; ++i) {
    zassert_false(guard.update(edge), "exactly the cutoff is not below it");
  }
  battery_status low{battery_guard::cutoff_millivolts - 1, charger_state::not_charging};
  zassert_false(guard.update(low));
  zassert_false(guard.update(low));
  zassert_false(guard.update({3000, charger_state::charging})); // Restarts the count.
  zassert_false(guard.update(low));
  zassert_false(guard.update(low));
  zassert_true(guard.update(low));
}

ZTEST(battery, test_start_needs_a_margin_above_the_cutoff) {
  zassert_false(battery_allows_start({3399, charger_state::not_charging}));
  zassert_true(battery_allows_start({3400, charger_state::not_charging}));
  zassert_true(battery_allows_start({3000, charger_state::charging}));
  zassert_false(battery_allows_start({3000, charger_state::fault}));
}
