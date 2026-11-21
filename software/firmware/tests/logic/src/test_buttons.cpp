#include "buttons.hpp"

#include <zephyr/ztest.h>

#include <optional>
#include <vector>

using namespace gtfog;
using namespace std::chrono_literals;

namespace {

// Drives the detector like the firmware does: a sample every 20 ms between `from` and `to`.
auto run(button_gestures &g, bool plus, bool minus, std::chrono::milliseconds from, std::chrono::milliseconds to) -> std::vector<button_event> {
  std::vector<button_event> events;
  for(auto t = from; t <= to; t += 20ms) {
    if(auto e = g.update(plus, minus, t)) {
      events.push_back(*e);
    }
  }
  return events;
}

} // namespace

ZTEST_SUITE(buttons, nullptr, nullptr, nullptr, nullptr, nullptr);

ZTEST(buttons, test_short_press_fires_on_release) {
  button_gestures g;
  zassert_true(run(g, true, false, 0ms, 100ms).empty());
  auto released = g.update(false, false, 120ms);
  zassert_true((released == button_event{button_action::plus, 1}));
  zassert_true(g.is_idle());
}

ZTEST(buttons, test_bounce_is_ignored) {
  button_gestures g;
  zassert_false(g.update(false, true, 0ms).has_value());
  zassert_false(g.update(false, false, 20ms).has_value());
}

ZTEST(buttons, test_long_press_repeats_and_accelerates) {
  button_gestures g;
  auto events = run(g, false, true, 0ms, 7000ms);
  zassert_false(events.empty());
  zassert_true((events.front() == button_event{button_action::minus, 1}));
  zassert_true((events.back() == button_event{button_action::minus, 3})); // 6..9 s of holding.
  // One repeat per 200 ms from 500 ms on.
  zassert_within(events.size(), (7000 - 500) / 200, 2);
  zassert_false(g.update(false, false, 7020ms).has_value(), "a long press must not also fire on release");
}

ZTEST(buttons, test_steps_are_capped) {
  button_gestures g;
  auto events = run(g, true, false, 0ms, 20s);
  zassert_equal(events.back().steps, button_timing{}.max_steps);
}

ZTEST(buttons, test_stuck_button_goes_quiet) {
  button_gestures g;
  std::ignore = run(g, true, false, 0ms, 30s);
  zassert_true(run(g, true, false, 30020ms, 40s).empty());
}

ZTEST(buttons, test_chord_toggles_strobe_once_then_powers_off) {
  button_gestures g;
  auto events = run(g, true, true, 0ms, 6s);
  zassert_equal(events.size(), 2);
  zassert_true((events[0] == button_event{button_action::toggle_strobe}));
  zassert_true((events[1] == button_event{button_action::power_off}));
}

ZTEST(buttons, test_chord_release_fires_nothing) {
  button_gestures g;
  std::ignore = run(g, true, true, 0ms, 2500ms);
  zassert_true(run(g, true, false, 2520ms, 4000ms).empty(), "the button still held must not repeat");
  zassert_false(g.update(false, false, 4020ms).has_value());
  zassert_true(g.is_idle());
}

ZTEST(buttons, test_chord_broken_before_threshold_does_not_fire) {
  button_gestures g;
  std::ignore = run(g, true, true, 0ms, 1000ms);
  std::ignore = g.update(true, false, 1020ms);
  zassert_true(run(g, true, true, 1040ms, 6s).empty());
}
