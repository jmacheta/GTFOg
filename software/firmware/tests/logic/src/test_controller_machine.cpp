#include "controller.hpp"

#include <zephyr/ztest.h>

#include <algorithm>
#include <vector>

using namespace gtfog;

namespace {

struct fake_outputs final : outputs {
  unsigned fan{999};
  bool strobe{true};
  std::vector<signal> signals;
  bool powered_off{false};
  unsigned saved{0};

  void set_fan(unsigned percent) override { fan = percent; }
  void set_strobe(bool on) override { strobe = on; }
  void show(signal s) override { signals.push_back(s); }
  void power_off() override { powered_off = true; }
  void settings_changed(device_settings const &) override { ++saved; }
};

auto press(button_action action, unsigned steps = 1) -> button_event { return {action, steps}; }

} // namespace

ZTEST_SUITE(controller_machine, nullptr, nullptr, nullptr, nullptr, nullptr);

ZTEST(controller_machine, test_initial_states) {
  fake_outputs out;
  controller c{out};
  zassert_true(c.is(states::running));
  zassert_true(c.is(states::unlocked));
  zassert_true(c.is(states::manual));
}

ZTEST(controller_machine, test_settings_move_the_lock_and_mode_states) {
  fake_outputs out;
  controller c{out};
  c.configure({.transport_lock = true, .mode = fan_mode::motion});
  zassert_true(c.is(states::locked));
  zassert_true(c.is(states::motion));
  c.update_settings({.transport_lock = false, .mode = fan_mode::motion});
  zassert_true(c.is(states::unlocked));
  zassert_true(c.is(states::motion));
  c.update_settings({.mode = fan_mode::manual});
  zassert_true(c.is(states::manual));
}

ZTEST(controller_machine, test_lock_blocks_fan_and_strobe_in_manual_mode_and_unlock_restores) {
  fake_outputs out;
  controller c{out};
  c.power_on();
  c.handle(press(button_action::plus, 4)); // 20 %
  c.set_strobe(true);
  zassert_equal(out.fan, 20);
  zassert_true(out.strobe);

  c.update_settings({.transport_lock = true});
  zassert_true(c.is(states::locked));
  zassert_equal(out.fan, 0);
  zassert_false(out.strobe);
  c.handle(press(button_action::plus));
  c.handle(press(button_action::minus));
  c.handle(press(button_action::toggle_strobe));
  zassert_equal(out.fan, 0);
  zassert_false(out.strobe);

  c.handle(press(button_action::unlock));
  zassert_true(c.is(states::unlocked));
  zassert_equal(out.fan, 20, "the targets were kept");
  zassert_true(out.strobe);
  zassert_equal(out.signals.back(), signal::unlocked);
  zassert_false(c.settings().transport_lock);
}

ZTEST(controller_machine, test_lock_blocks_fan_and_strobe_in_motion_mode_and_unlock_restores) {
  fake_outputs out;
  controller c{out};
  c.update_settings({.mode = fan_mode::motion, .motion_speed = {30, 50, 80}});
  c.power_on();
  c.set_strobe(true);
  zassert_equal(out.fan, 30);

  c.update_settings({.transport_lock = true, .mode = fan_mode::motion, .motion_speed = {30, 50, 80}});
  zassert_true(c.is(states::locked));
  zassert_true(c.is(states::motion));
  c.observe(activity_level::running);
  c.observe(activity_level::running);
  c.handle(press(button_action::plus));
  c.handle(press(button_action::toggle_strobe));
  zassert_equal(out.fan, 0);
  zassert_false(out.strobe);

  c.handle(press(button_action::unlock));
  zassert_true(c.is(states::unlocked));
  zassert_true(c.is(states::motion));
  zassert_equal(out.fan, 80, "the level measured under the lock applies again");
  zassert_true(out.strobe);
}

ZTEST(controller_machine, test_locked_buttons_signal_once_and_do_not_save) {
  fake_outputs out;
  controller c{out};
  c.configure({.transport_lock = true});
  c.power_on();
  out.signals.clear();
  c.handle(button_event{button_action::minus, 1, false});
  c.handle(button_event{button_action::minus, 1, true});
  c.handle(button_event{button_action::toggle_strobe, 1, false});
  zassert_true((out.signals == std::vector{signal::locked, signal::locked}));
  zassert_equal(out.saved, 0);
}

ZTEST(controller_machine, test_power_off_from_every_lock_and_mode_state) {
  for(bool lock : {false, true}) {
    for(auto mode : {fan_mode::manual, fan_mode::motion}) {
      fake_outputs out;
      controller c{out};
      c.configure({.transport_lock = lock, .mode = mode});
      c.power_on();
      c.set_strobe(true);
      c.handle(press(button_action::power_off));
      zassert_true(c.is(states::powered_off));
      zassert_true(out.powered_off);
      zassert_equal(out.fan, 0);
      zassert_false(out.strobe);
      zassert_equal(out.signals.back(), signal::power_off);
    }
  }
}

ZTEST(controller_machine, test_power_off_by_battery_and_auto_off) {
  fake_outputs low;
  controller c{low};
  c.check_start({3000, charger_state::not_charging});
  zassert_true(c.is(states::powered_off));
  zassert_equal(low.signals.back(), signal::low_battery);

  fake_outputs idle;
  controller d{idle};
  d.configure({.auto_off_minutes = 1});
  d.elapsed(61s, false);
  zassert_true(d.is(states::powered_off));
}

ZTEST(controller_machine, test_powered_off_ignores_further_requests) {
  fake_outputs out;
  controller c{out};
  c.handle(press(button_action::power_off));
  out.signals.clear();
  c.set_fan_speed(50);
  c.set_strobe(true);
  c.report(signal::fault);
  zassert_true(c.is(states::powered_off));
  zassert_equal(out.fan, 0);
  zassert_false(out.strobe);
  zassert_true(out.signals.empty());
}

ZTEST(controller_machine, test_motion_and_manual_switching) {
  fake_outputs out;
  controller c{out};
  c.update_settings({.mode = fan_mode::motion, .motion_delay_s = 10, .motion_speed = {20, 50, 80}});
  c.power_on();
  c.observe(activity_level::walking);
  c.observe(activity_level::walking);
  zassert_true(c.is(states::motion));
  zassert_equal(out.fan, 50);
  zassert_equal(c.activity_code(), 1);

  c.update_settings({.mode = fan_mode::manual, .motion_speed = {20, 50, 80}});
  zassert_true(c.is(states::manual));
  zassert_equal(c.activity_code(), 255);
  zassert_equal(out.fan, 0, "the manual target is untouched");
  c.observe(activity_level::running); // Ignored in manual mode.
  c.observe(activity_level::running);
  zassert_equal(out.fan, 0);

  c.update_settings({.mode = fan_mode::motion, .motion_speed = {20, 50, 80}});
  zassert_true(c.is(states::motion));
  zassert_equal(c.activity_code(), 0, "back to still");
  zassert_equal(out.fan, 20);
}

ZTEST(controller_machine, test_sensor_lost_in_motion_mode_runs_at_the_still_speed) {
  fake_outputs out;
  controller c{out};
  c.update_settings({.mode = fan_mode::motion, .motion_speed = {20, 50, 80}});
  c.observe(activity_level::running);
  c.observe(activity_level::running);
  zassert_equal(out.fan, 80);
  c.sensor_lost();
  zassert_equal(out.fan, 20);
  zassert_equal(out.signals.back(), signal::fault);
  zassert_true(c.is(states::running));
}

ZTEST(controller_machine, test_battery_warning_once_until_the_cell_recovers) {
  fake_outputs out;
  controller c{out};
  auto count = [&] { return std::count(out.signals.begin(), out.signals.end(), signal::battery_warning); };
  c.handle(battery_status{3500, charger_state::not_charging}); // 5 %.
  c.handle(battery_status{3500, charger_state::not_charging});
  zassert_equal(count(), 1);
  c.handle(battery_status{3700, charger_state::not_charging}); // 30 %: recovered.
  c.handle(battery_status{3500, charger_state::not_charging});
  zassert_equal(count(), 2);
  zassert_true(c.is(states::running));
}
