#include "gestures.hpp"

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

ZTEST_SUITE(gestures, nullptr, nullptr, nullptr, nullptr, nullptr);

ZTEST(gestures, test_short_press_fires_on_release) {
  button_gestures g;
  zassert_true(run(g, true, false, 0ms, 100ms).empty());
  auto released = g.update(false, false, 120ms);
  zassert_true((released == button_event{button_action::plus, 1}));
  zassert_true(g.is_idle());
}

ZTEST(gestures, test_bounce_is_ignored) {
  button_gestures g;
  zassert_false(g.update(false, true, 0ms).has_value());
  zassert_false(g.update(false, false, 20ms).has_value());
}

ZTEST(gestures, test_long_press_repeats_and_accelerates) {
  button_gestures g;
  auto events = run(g, false, true, 0ms, 7000ms);
  zassert_false(events.empty());
  zassert_true((events.front() == button_event{button_action::minus, 1}));
  zassert_true((events.back() == button_event{button_action::minus, 3, true})); // 6..9 s of holding.
  // One repeat per 200 ms from 500 ms on.
  zassert_within(events.size(), (7000 - 500) / 200, 2);
  zassert_false(g.update(false, false, 7020ms).has_value(), "a long press must not also fire on release");
}

ZTEST(gestures, test_steps_are_capped) {
  button_gestures g;
  auto events = run(g, true, false, 0ms, 20s);
  zassert_equal(events.back().steps, button_timing{}.max_steps);
}

ZTEST(gestures, test_stuck_button_goes_quiet) {
  button_gestures g;
  std::ignore = run(g, true, false, 0ms, 30s);
  zassert_true(run(g, true, false, 30020ms, 40s).empty());
}

ZTEST(gestures, test_chord_released_between_2_and_5_s_toggles_strobe) {
  button_gestures g;
  zassert_true(run(g, true, true, 0ms, 3s).empty());
  auto released = g.update(false, false, 3020ms);
  zassert_true((released == button_event{button_action::toggle_strobe}));
  zassert_false(g.update(false, false, 3040ms).has_value());
}

ZTEST(gestures, test_chord_toggles_strobe_when_one_button_is_let_go) {
  button_gestures g;
  std::ignore = run(g, true, true, 0ms, 2500ms);
  auto released = g.update(true, false, 2520ms);
  zassert_true((released == button_event{button_action::toggle_strobe}));
  zassert_true(run(g, true, false, 2540ms, 4000ms).empty(), "the button still held must not repeat");
  zassert_false(g.update(false, false, 4020ms).has_value());
  zassert_true(g.is_idle());
}

ZTEST(gestures, test_chord_held_to_5_s_powers_off_without_strobe) {
  button_gestures g;
  auto events = run(g, true, true, 0ms, 6s);
  zassert_equal(events.size(), 1);
  zassert_true((events[0] == button_event{button_action::power_off}));
  zassert_false(g.update(false, false, 6020ms).has_value());
}

ZTEST(gestures, test_chord_released_before_2_s_fires_nothing) {
  button_gestures g;
  std::ignore = run(g, true, true, 0ms, 1000ms);
  zassert_false(g.update(true, false, 1020ms).has_value());
  zassert_true(run(g, true, true, 1040ms, 6s).empty());
}

// Presses plus for 100 ms starting at `at`.
auto tap(button_gestures &g, std::chrono::milliseconds at) -> std::optional<button_event> {
  std::ignore = g.update(true, false, at);
  std::ignore = g.update(true, false, at + 60ms);
  return g.update(false, false, at + 100ms);
}

ZTEST(gestures, test_third_quick_plus_press_unlocks) {
  button_gestures g;
  zassert_true((tap(g, 0ms) == button_event{button_action::plus}));
  zassert_true((tap(g, 400ms) == button_event{button_action::plus}));
  zassert_true((tap(g, 800ms) == button_event{button_action::unlock}));
  zassert_true((tap(g, 1200ms) == button_event{button_action::plus}), "the count starts over");
}

ZTEST(gestures, test_minus_tap_resets_the_unlock_count) {
  button_gestures g;
  zassert_true((tap(g, 0ms) == button_event{button_action::plus}));
  zassert_true((tap(g, 300ms) == button_event{button_action::plus}));
  std::ignore = g.update(false, true, 600ms);
  std::ignore = g.update(false, true, 660ms);
  zassert_true((g.update(false, false, 700ms) == button_event{button_action::minus}));
  zassert_true((tap(g, 900ms) == button_event{button_action::plus}), "the third plus tap would have unlocked");
  zassert_true((tap(g, 1200ms) == button_event{button_action::plus}));
  zassert_true((tap(g, 1500ms) == button_event{button_action::unlock}), "three plus taps after the minus tap unlock");
}

ZTEST(gestures, test_slow_plus_presses_do_not_unlock) {
  button_gestures g;
  for(auto at = 0ms; at < 6s; at += 1500ms) {
    zassert_true((tap(g, at) == button_event{button_action::plus}));
  }
}

ZTEST(gestures, test_long_press_does_not_count_for_unlock) {
  button_gestures g;
  zassert_true((tap(g, 0ms) == button_event{button_action::plus}));
  zassert_true(run(g, true, false, 400ms, 1000ms).size() > 0);
  std::ignore = g.update(false, false, 1020ms);
  zassert_true((tap(g, 1200ms) == button_event{button_action::plus}));
}

ZTEST(gestures, test_release_between_long_press_and_first_repeat_is_one_step) {
  button_gestures g;
  std::ignore = g.update(true, false, 0ms);
  zassert_false(g.update(true, false, 490ms).has_value());
  zassert_true((g.update(false, false, 505ms) == button_event{button_action::plus, 1}));
  zassert_false(g.update(false, false, 520ms).has_value());
  // After a repeat has fired, the release adds nothing.
  button_gestures h;
  auto events = run(h, false, true, 0ms, 520ms);
  zassert_equal(events.size(), 1);
  zassert_false(h.update(false, false, 530ms).has_value());
}

ZTEST(gestures, test_press_held_since_boot_is_ignored_until_released) {
  button_gestures g;
  g.wait_for_release();
  zassert_true(run(g, true, false, 0ms, 3s).empty());
  zassert_false(g.update(false, false, 3020ms).has_value(), "the release is no short press");
  zassert_true((tap(g, 4s) == button_event{button_action::plus}));
}

ZTEST(gestures, test_stuck_button_lets_the_thread_sleep) {
  button_gestures g;
  std::ignore = run(g, true, false, 0ms, 29s);
  zassert_false(g.is_idle());
  std::ignore = run(g, true, false, 29020ms, 30100ms);
  zassert_true(g.is_idle());
  zassert_false(g.update(false, false, 40s).has_value());
}

ZTEST(gestures, test_stuck_button_then_chord_still_powers_off) {
  button_gestures g;
  std::ignore = run(g, true, false, 0ms, 31s);
  zassert_true(g.is_idle());
  auto events = run(g, true, true, 31020ms, 37s);
  zassert_equal(events.size(), 1);
  zassert_true((events[0] == button_event{button_action::power_off}));
}

ZTEST(gestures, test_second_button_during_repeat_starts_a_chord_and_stops_the_repeat) {
  button_gestures g;
  zassert_false(run(g, true, false, 0ms, 1000ms).empty());
  zassert_true(run(g, true, true, 1020ms, 3200ms).empty());
  auto released = g.update(false, false, 3220ms);
  zassert_true((released == button_event{button_action::toggle_strobe}));
}

ZTEST(gestures, test_both_held_since_boot_still_power_off) {
  button_gestures g;
  g.wait_for_release();
  auto events = run(g, true, true, 0ms, 6s);
  zassert_equal(events.size(), 1);
  zassert_true((events[0] == button_event{button_action::power_off}));
}

ZTEST(gestures, test_second_button_after_a_boot_press_starts_the_chord) {
  button_gestures g;
  g.wait_for_release();
  zassert_true(run(g, true, false, 0ms, 1s).empty());
  auto events = run(g, true, true, 1020ms, 7s);
  zassert_equal(events.size(), 1);
  zassert_true((events[0] == button_event{button_action::power_off}));
}

ZTEST(gestures, test_only_repeats_after_the_first_are_marked) {
  button_gestures g;
  auto events = run(g, true, false, 0ms, 2s);
  zassert_true(events.size() > 3);
  zassert_false(events.front().repeat);
  for(std::size_t i = 1; i != events.size(); ++i) {
    zassert_true(events[i].repeat);
  }
}
