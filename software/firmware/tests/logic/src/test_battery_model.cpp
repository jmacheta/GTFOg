#include "battery_model.hpp"

#include <zephyr/ztest.h>

#include <initializer_list>

using namespace gtfog;

ZTEST_SUITE(battery_model, nullptr, nullptr, nullptr, nullptr, nullptr);

ZTEST(battery_model, test_percentage_is_clamped_and_monotonic) {
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

ZTEST(battery_model, test_percentage_interpolates) {
  zassert_equal(battery_percentage(3750), 40); // Halfway between 3700 mV (30 %) and 3800 mV (50 %).
}

ZTEST(battery_model, test_charger_states) {
  zassert_equal(decode_charger_state(true, true), charger_state::not_charging);
  zassert_equal(decode_charger_state(true, false), charger_state::charging);
  zassert_equal(decode_charger_state(false, true), charger_state::fault);
  zassert_equal(decode_charger_state(false, false), charger_state::fault);
}

ZTEST(battery_model, test_guard_needs_consecutive_low_samples) {
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

ZTEST(battery_model, test_guard_ignores_low_voltage_while_charging) {
  battery_guard guard;
  for(int i = 0; i != 10; ++i) {
    zassert_false(guard.update({3000, charger_state::charging}));
  }
}

ZTEST(battery_model, test_guard_cutoff_boundary_and_charging_sample_in_between) {
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

ZTEST(battery_model, test_start_needs_a_margin_above_the_cutoff) {
  zassert_false(battery_allows_start({3399, charger_state::not_charging}));
  zassert_true(battery_allows_start({3400, charger_state::not_charging}));
  zassert_true(battery_allows_start({3000, charger_state::charging}));
  zassert_false(battery_allows_start({3000, charger_state::fault}));
}

ZTEST(battery_model, test_percentage_at_every_knot_of_the_curve) {
  struct knot {
    int millivolts;
    unsigned percent;
  };
  for(auto k : {knot{3300, 0}, knot{3500, 5}, knot{3600, 10}, knot{3700, 30}, knot{3800, 50}, knot{3900, 65}, knot{4000, 80}, knot{4200, 100}}) {
    zassert_equal(battery_percentage(k.millivolts), k.percent, "%d mV", k.millivolts);
  }
}

ZTEST(battery_model, test_read_failures_show_a_fault_once_then_power_off_once) {
  using verdict = read_failure_guard::verdict;
  read_failure_guard guard;
  for(unsigned i = 1; i < read_failure_guard::fault_reads; ++i) {
    zassert_equal(guard.update(false), verdict::ok);
  }
  zassert_equal(guard.update(false), verdict::fault);
  for(unsigned i = read_failure_guard::fault_reads + 1; i < read_failure_guard::power_off_reads; ++i) {
    zassert_equal(guard.update(false), verdict::ok);
  }
  zassert_equal(guard.update(false), verdict::power_off);
  zassert_equal(guard.update(false), verdict::ok, "reported once");
  zassert_equal(guard.update(true), verdict::ok);
  zassert_equal(guard.update(false), verdict::ok);
  zassert_equal(guard.update(false), verdict::ok);
  zassert_equal(guard.update(false), verdict::fault, "a good read rearms it");
}
