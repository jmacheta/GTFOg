#include "controller.hpp"
#include "fake_outputs.hpp"

#include <zephyr/ztest.h>

#include <algorithm>
#include <vector>

using namespace gtfog;
using namespace std::chrono_literals;

namespace {

auto press(button_action action, unsigned steps = 1) -> button_event { return {action, steps}; }

} // namespace

ZTEST_SUITE(controller_machine, nullptr, nullptr, nullptr, nullptr, nullptr);

ZTEST(controller_machine, test_initial_states) {
  fake_outputs out;
  controller c{out};
  zassert_true(c.is(states::running));
  zassert_false(c.settings().transport_lock);
  zassert_equal(c.settings().mode, fan_mode::manual);
}

// The lock and mode are written once, in the settings: what comes out is what went in, whichever way it was written.
ZTEST(controller_machine, test_settings_round_trip_through_the_machine_and_the_codec) {
  for(bool lock_a : {false, true}) {
    for(auto mode_a : {fan_mode::manual, fan_mode::motion}) {
      for(bool lock_b : {false, true}) {
        for(auto mode_b : {fan_mode::manual, fan_mode::motion}) {
          fake_outputs out;
          controller c{out};
          device_settings loaded{.transport_lock = lock_a, .mode = mode_a};
          device_settings written{.transport_lock = lock_b, .mode = mode_b};
          c.configure(loaded); // Flash load at boot.
          zassert_true(c.settings() == loaded);
          c.update_settings(written); // Bluetooth.
          zassert_true(c.settings() == written);
          auto bytes = encode(c.settings());
          auto decoded = decode_settings(bytes);
          zassert_true(decoded.has_value() && *decoded == written);
          c.configure(loaded);
          c.handle(press(button_action::unlock));
          zassert_false(c.settings().transport_lock);
        }
      }
    }
  }
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
  zassert_true(c.settings().transport_lock);
  zassert_equal(out.fan, 0);
  zassert_false(out.strobe);
  c.handle(press(button_action::plus));
  c.handle(press(button_action::minus));
  c.handle(press(button_action::toggle_strobe));
  zassert_equal(out.fan, 0);
  zassert_false(out.strobe);

  c.handle(press(button_action::unlock));
  zassert_false(c.settings().transport_lock);
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
  zassert_true(c.settings().transport_lock);
  zassert_equal(c.settings().mode, fan_mode::motion);
  c.observe(activity_level::running);
  c.observe(activity_level::running);
  c.handle(press(button_action::plus));
  c.handle(press(button_action::toggle_strobe));
  zassert_equal(out.fan, 0);
  zassert_false(out.strobe);

  c.handle(press(button_action::unlock));
  zassert_false(c.settings().transport_lock);
  zassert_equal(c.settings().mode, fan_mode::motion);
  zassert_equal(out.fan, 30, "the accelerometer was off under the lock, so the level starts again at still");
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
  zassert_equal(out.saved_count(), 0);
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
  zassert_equal(c.settings().mode, fan_mode::motion);
  zassert_equal(out.fan, 50);
  zassert_equal(c.activity_code(), 1);

  c.update_settings({.mode = fan_mode::manual, .motion_speed = {20, 50, 80}});
  zassert_equal(c.settings().mode, fan_mode::manual);
  zassert_equal(c.activity_code(), 255);
  zassert_equal(out.fan, 0, "the manual target is untouched");
  c.observe(activity_level::running); // Ignored in manual mode.
  c.observe(activity_level::running);
  zassert_equal(out.fan, 0);

  c.update_settings({.mode = fan_mode::motion, .motion_speed = {20, 50, 80}});
  zassert_equal(c.settings().mode, fan_mode::motion);
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
  auto sample = [&](int millivolts) {
    for(int i = 0; i != 3; ++i) { // The warning needs 3 samples in a row.
      c.handle(battery_status{millivolts, charger_state::not_charging});
    }
  };
  sample(3500); // 5 %.
  zassert_equal(count(), 1);
  sample(3700); // 30 %: recovered.
  sample(3500);
  zassert_equal(count(), 2);
  zassert_true(c.is(states::running));
}

ZTEST(controller_machine, test_activity_level_restarts_when_the_lock_flips_by_gesture_or_bluetooth) {
  for(bool by_gesture : {true, false}) {
    fake_outputs out;
    controller c{out};
    c.update_settings({.mode = fan_mode::motion, .motion_speed = {30, 50, 80}});
    c.power_on();
    c.observe(activity_level::running);
    c.observe(activity_level::running);
    zassert_equal(out.fan, 80);
    c.update_settings({.transport_lock = true, .mode = fan_mode::motion, .motion_speed = {30, 50, 80}});
    zassert_equal(out.fan, 0);
    if(by_gesture) {
      c.handle(press(button_action::unlock));
    } else {
      c.update_settings({.mode = fan_mode::motion, .motion_speed = {30, 50, 80}});
    }
    zassert_equal(out.fan, 30);
    zassert_equal(c.activity_code(), 0);
  }
}

ZTEST(controller_machine, test_unreadable_battery_powers_off) {
  fake_outputs out;
  controller c{out};
  c.power_on();
  c.battery_unreadable();
  zassert_true(c.is(states::powered_off));
  zassert_true(out.powered_off);
  zassert_equal(out.signals.back(), signal::low_battery);
}

// The lock and mode regions hold the truth; every way in leads to the matching state.
ZTEST(controller_machine, test_lock_and_mode_states_follow_flash_load_bluetooth_and_the_unlock_gesture) {
  fake_outputs out;
  controller c{out};
  zassert_true(c.is(states::unlocked) && c.is(states::manual));

  c.configure({.transport_lock = true, .mode = fan_mode::motion}); // Flash load: states change, nothing is shown or saved.
  zassert_true(c.is(states::locked) && c.is(states::motion));
  zassert_equal(out.saved_count(), 0);
  zassert_true(out.signals.empty());

  c.handle(press(button_action::unlock));
  zassert_true(c.is(states::unlocked) && c.is(states::motion));
  zassert_equal(out.saved_count(), 1);
  zassert_equal(out.signals.back(), signal::unlocked);

  c.update_settings({.transport_lock = true, .mode = fan_mode::manual}); // Bluetooth.
  zassert_true(c.is(states::locked) && c.is(states::manual));
  zassert_equal(out.saved_count(), 2);
  c.update_settings({.stealth = true, .transport_lock = true}); // Same states: stay.
  zassert_true(c.is(states::locked) && c.is(states::manual));
  c.update_settings({});
  zassert_true(c.is(states::unlocked) && c.is(states::manual));
  zassert_equal(out.saved_count(), 4);
}

ZTEST(controller_machine, test_encoded_flags_come_from_the_states) {
  fake_outputs out;
  controller c{out};
  c.update_settings({.transport_lock = true, .mode = fan_mode::motion});
  auto bytes = encode(c.settings());
  zassert_equal(bytes[0] & 2U, 2U);
  zassert_equal(bytes[3], 1);
  c.handle(press(button_action::unlock)); // Only the state changes; the flag follows it.
  zassert_true(c.is(states::unlocked));
  bytes = encode(c.settings());
  zassert_equal(bytes[0] & 2U, 0U);
  zassert_equal(bytes[3], 1);
}
