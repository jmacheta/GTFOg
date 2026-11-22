#include "controller.hpp"

#include <zephyr/ztest.h>

#include <algorithm>
#include <array>
#include <span>
#include <vector>

using namespace gtfog;

namespace {

struct fake_outputs final : outputs {
  unsigned fan{999};
  bool strobe{true};
  std::vector<signal> signals;
  bool powered_off{false};
  std::vector<device_settings> saved;
  unsigned fan_calls{0};

  void set_fan(unsigned percent) override {
    fan = percent;
    ++fan_calls;
  }
  void set_strobe(bool on) override { strobe = on; }
  void show(signal s) override { signals.push_back(s); }
  void power_off() override { powered_off = true; }
  void settings_changed(device_settings const &s) override { saved.push_back(s); }
};

} // namespace

ZTEST_SUITE(controller, nullptr, nullptr, nullptr, nullptr, nullptr);

ZTEST(controller, test_power_on_stops_everything) {
  fake_outputs out;
  controller c{out};
  c.power_on();
  zassert_equal(out.fan, 0);
  zassert_false(out.strobe);
  zassert_true(out.signals == std::vector{signal::power_on});
}

ZTEST(controller, test_buttons_change_speed_within_limits) {
  fake_outputs out;
  controller c{out};
  c.configure({.fan_step = 1});
  c.power_on();

  c.handle(button_event{button_action::plus, 5});
  zassert_equal(out.fan, 5);
  zassert_equal(out.signals.back(), signal::speed_up);

  c.handle(button_event{button_action::minus, 2});
  zassert_equal(out.fan, 3);
  zassert_equal(out.signals.back(), signal::speed_down);

  c.handle(button_event{button_action::minus, 5});
  zassert_equal(out.fan, 0);
  zassert_equal(out.signals.back(), signal::speed_limit);

  for(int i = 0; i != 30; ++i) {
    c.handle(button_event{button_action::plus, 5});
  }
  zassert_equal(out.fan, 100);
  zassert_equal(c.fan_speed(), 100);
  zassert_equal(out.signals.back(), signal::speed_limit);
}

ZTEST(controller, test_remote_speed_is_clamped) {
  fake_outputs out;
  controller c{out};
  c.set_fan_speed(250);
  zassert_equal(out.fan, 100);
}

ZTEST(controller, test_strobe_toggles) {
  fake_outputs out;
  controller c{out};
  c.power_on();
  c.handle(button_event{button_action::toggle_strobe});
  zassert_true(out.strobe);
  zassert_true(c.strobe());
  c.handle(button_event{button_action::toggle_strobe});
  zassert_false(out.strobe);
}

ZTEST(controller, test_power_off_stops_outputs_first) {
  fake_outputs out;
  controller c{out};
  c.set_fan_speed(60);
  c.set_strobe(true);
  c.handle(button_event{button_action::power_off});
  zassert_equal(out.fan, 0);
  zassert_false(out.strobe);
  zassert_true(out.powered_off);
}

ZTEST(controller, test_low_battery_powers_off) {
  fake_outputs out;
  controller c{out};
  for(int i = 0; i != 3; ++i) {
    c.handle(battery_status{3100, charger_state::not_charging});
  }
  zassert_true(out.powered_off);
  zassert_equal(out.signals.back(), signal::low_battery);
  zassert_equal(c.battery_level(), 0);
}

ZTEST(controller, test_fan_duty_curve) {
  static_assert(fan_duty_permille(0, 200) == 0);
  static_assert(fan_duty_permille(1, 200) == 200);
  static_assert(fan_duty_permille(100, 200) == 1000);
  static_assert(fan_duty_permille(150, 200) == 1000);
  unsigned previous = 0;
  for(unsigned p = 1; p <= 100; ++p) {
    zassert_true(fan_duty_permille(p, 200) > previous);
    previous = fan_duty_permille(p, 200);
  }
}

ZTEST(controller, test_stealth_shows_nothing) {
  fake_outputs out;
  controller c{out};
  c.configure({.stealth = true});
  c.power_on();
  c.handle(button_event{button_action::plus, 1});
  c.handle(button_event{button_action::power_off});
  zassert_true(out.signals.empty());
  zassert_true(out.powered_off);
}

ZTEST(controller, test_transport_lock_keeps_fan_and_strobe_off) {
  fake_outputs out;
  controller c{out};
  c.configure({.transport_lock = true});
  c.power_on();
  c.handle(button_event{button_action::plus, 5});
  c.handle(button_event{button_action::toggle_strobe});
  zassert_equal(out.fan, 0);
  zassert_false(out.strobe);
  zassert_equal(out.signals.back(), signal::locked);
  c.set_fan_speed(40); // The phone sets the target, but the lock keeps the outputs off.
  c.set_strobe(true);
  zassert_equal(out.fan, 0);
  zassert_false(out.strobe);
  zassert_equal(c.fan_speed(), 0);
  zassert_false(c.strobe());
  c.handle(button_event{button_action::power_off});
  zassert_true(out.powered_off);
  zassert_equal(out.signals.back(), signal::power_off);
}

ZTEST(controller, test_auto_off_after_idle_time) {
  fake_outputs out;
  controller c{out};
  c.configure({.auto_off_minutes = 1});
  c.power_on();
  c.elapsed(50, false);
  c.handle(button_event{button_action::minus, 1}); // A button press restarts the count.
  c.elapsed(50, false);
  zassert_false(out.powered_off);
  c.elapsed(10, true); // A connected phone keeps it on.
  c.set_fan_speed(10);
  c.elapsed(120, false); // So does a running fan.
  zassert_false(out.powered_off);
  c.set_fan_speed(0);
  c.elapsed(50, false);
  zassert_false(out.powered_off);
  c.elapsed(10, false);
  zassert_true(out.powered_off);
}

ZTEST(controller, test_auto_off_disabled) {
  fake_outputs out;
  controller c{out};
  c.power_on();
  c.elapsed(1'000'000, false);
  zassert_false(out.powered_off);
}

ZTEST(controller, test_settings_encoding) {
  device_settings s{.stealth = true,
                    .transport_lock = true,
                    .auto_off_minutes = 15,
                    .fan_step = 10,
                    .mode = fan_mode::motion,
                    .motion_delay_s = 30,
                    .motion_speed = {40, 60, 80}};
  auto bytes = encode(s);
  zassert_equal(bytes.size(), 8);
  std::array<std::uint8_t, 8> const expected{3, 15, 10, 1, 30, 40, 60, 80};
  zassert_true(bytes == expected);
  zassert_true(decode_settings(bytes) == s);
  zassert_equal(encode({})[2], 5);
  zassert_equal(encode({})[4], 60);
  zassert_equal(encode({})[5], 20);
  zassert_equal(encode({})[6], 50);
  zassert_equal(encode({})[7], 100);
}

ZTEST(controller, test_settings_decoding_accepts_older_formats) {
  std::uint8_t const v1[] = {3, 15};
  auto old = decode_settings(v1);
  zassert_true(old.has_value());
  zassert_true((*old == device_settings{.stealth = true, .transport_lock = true, .auto_off_minutes = 15}));
  zassert_equal(old->fan_step, 5);

  std::uint8_t const v2[] = {0, 0, 10, 1, 30, 40}; // The single idle speed becomes the still speed.
  auto v2s = decode_settings(v2);
  zassert_true(v2s.has_value());
  zassert_true((*v2s == device_settings{.fan_step = 10, .mode = fan_mode::motion, .motion_delay_s = 30, .motion_speed = {40, 50, 100}}));
}

ZTEST(controller, test_settings_decoding_rejects_bad_values) {
  std::uint8_t const too_short[] = {1};
  std::uint8_t const three[] = {1, 0, 5};
  std::uint8_t const seven[] = {0, 0, 5, 0, 60, 20, 50};
  std::uint8_t const unknown_flag[] = {4, 0, 5, 0, 60, 20, 50, 100};
  std::uint8_t const step_zero[] = {0, 0, 0, 0, 60, 20, 50, 100};
  std::uint8_t const step_big[] = {0, 0, 26, 0, 60, 20, 50, 100};
  std::uint8_t const mode_two[] = {0, 0, 5, 2, 60, 20, 50, 100};
  std::uint8_t const delay_short[] = {0, 0, 5, 1, 4, 20, 50, 100};
  std::uint8_t const still_big[] = {0, 0, 5, 1, 60, 101, 50, 100};
  std::uint8_t const walking_big[] = {0, 0, 5, 1, 60, 20, 101, 100};
  std::uint8_t const running_big[] = {0, 0, 5, 1, 60, 20, 50, 101};
  std::uint8_t const v2_speed_big[] = {0, 0, 5, 1, 60, 101};
  std::uint8_t const v1_with_unknown_flag[] = {4, 0};
  std::uint8_t const ok_limits[] = {0, 0, 25, 1, 5, 100, 0, 100};
  for(auto bad : {std::span<std::uint8_t const>{too_short}, std::span<std::uint8_t const>{three}, std::span<std::uint8_t const>{seven},
                  std::span<std::uint8_t const>{unknown_flag}, std::span<std::uint8_t const>{step_zero}, std::span<std::uint8_t const>{step_big},
                  std::span<std::uint8_t const>{mode_two}, std::span<std::uint8_t const>{delay_short}, std::span<std::uint8_t const>{still_big},
                  std::span<std::uint8_t const>{walking_big}, std::span<std::uint8_t const>{running_big}, std::span<std::uint8_t const>{v2_speed_big},
                  std::span<std::uint8_t const>{v1_with_unknown_flag}}) {
    zassert_false(decode_settings(bad).has_value());
  }
  zassert_true(decode_settings(ok_limits).has_value());
}

ZTEST(controller, test_fan_step_scales_clicks) {
  fake_outputs out;
  controller c{out};
  c.power_on();
  c.handle(button_event{button_action::plus, 1});
  zassert_equal(out.fan, 5);
  c.handle(button_event{button_action::plus, 2});
  zassert_equal(out.fan, 15);
  c.configure({.fan_step = 25});
  c.handle(button_event{button_action::plus, 1});
  zassert_equal(out.fan, 40);
  c.handle(button_event{button_action::minus, 5});
  zassert_equal(out.fan, 0);
  zassert_equal(out.signals.back(), signal::speed_limit);
  zassert_true(out.saved.empty());
}

ZTEST(controller, test_unlock_gesture_clears_transport_lock) {
  fake_outputs out;
  controller c{out};
  c.configure({.transport_lock = true, .fan_step = 5});
  c.power_on();
  c.handle(button_event{button_action::unlock});
  zassert_false(c.settings().transport_lock);
  zassert_equal(out.signals.back(), signal::unlocked);
  zassert_equal(out.saved.size(), 1);
  zassert_false(out.saved[0].transport_lock);
  zassert_equal(out.fan, 0);
  c.handle(button_event{button_action::plus, 1});
  zassert_equal(out.fan, 5);
}

ZTEST(controller, test_unlock_gesture_is_plus_when_not_locked) {
  fake_outputs out;
  controller c{out};
  c.power_on();
  c.handle(button_event{button_action::unlock});
  zassert_equal(out.fan, 5);
  zassert_equal(out.signals.back(), signal::speed_up);
  zassert_true(out.saved.empty());
}

ZTEST(controller, test_user_settings_are_stored_and_flash_settings_are_not) {
  fake_outputs out;
  controller c{out};
  c.configure({.auto_off_minutes = 3});
  zassert_true(out.saved.empty());
  c.update_settings({.auto_off_minutes = 7});
  zassert_equal(out.saved.size(), 1);
  zassert_equal(out.saved[0].auto_off_minutes, 7);
  zassert_equal(c.settings().auto_off_minutes, 7);
}

namespace {

// One raw level per second for `seconds` seconds.
void feed(controller &c, activity_level raw, unsigned seconds) {
  for(unsigned i = 0; i != seconds; ++i) {
    c.observe(raw);
  }
}

} // namespace

ZTEST(controller, test_motion_mode_runs_the_target_of_the_level) {
  fake_outputs out;
  controller c{out};
  c.update_settings({.mode = fan_mode::motion, .motion_delay_s = 10, .motion_speed = {20, 50, 80}});
  c.power_on();
  zassert_equal(out.fan, 20);
  zassert_equal(c.activity_code(), 0);
  feed(c, activity_level::walking, 1);
  zassert_equal(out.fan, 20, "one second is not enough");
  feed(c, activity_level::walking, 1);
  zassert_equal(out.fan, 50);
  zassert_equal(c.activity_code(), 1);
  feed(c, activity_level::running, 2);
  zassert_equal(out.fan, 80);
  zassert_equal(c.fan_speed(), 80);
  zassert_equal(c.activity_code(), 2);
}

ZTEST(controller, test_motion_mode_rising_needs_consecutive_seconds_and_goes_to_the_lower_level) {
  fake_outputs out;
  controller c{out};
  c.update_settings({.mode = fan_mode::motion, .motion_delay_s = 10, .motion_speed = {20, 50, 80}});
  feed(c, activity_level::running, 1);
  feed(c, activity_level::still, 1);
  feed(c, activity_level::running, 1);
  zassert_equal(c.activity_code(), 0, "a still second resets the count");
  feed(c, activity_level::running, 1);
  zassert_equal(c.activity_code(), 2);

  controller d{out};
  d.update_settings({.mode = fan_mode::motion});
  feed(d, activity_level::running, 1);
  feed(d, activity_level::walking, 1);
  zassert_equal(d.activity_code(), 1, "running then walking rises to walking");
}

ZTEST(controller, test_motion_mode_slows_down_after_the_delay) {
  fake_outputs out;
  controller c{out};
  c.update_settings({.mode = fan_mode::motion, .motion_delay_s = 10, .motion_speed = {20, 50, 80}});
  feed(c, activity_level::running, 2);
  feed(c, activity_level::walking, 9);
  zassert_equal(out.fan, 80);
  feed(c, activity_level::running, 1); // Back at the level: the countdown restarts.
  feed(c, activity_level::walking, 9);
  zassert_equal(out.fan, 80);
  feed(c, activity_level::walking, 1);
  zassert_equal(out.fan, 50);
  feed(c, activity_level::still, 10);
  zassert_equal(out.fan, 20);
  zassert_equal(c.activity_code(), 0);
}

ZTEST(controller, test_motion_mode_buttons_and_remote_change_the_target_of_the_level) {
  fake_outputs out;
  controller c{out};
  c.update_settings({.fan_step = 10, .mode = fan_mode::motion, .motion_delay_s = 10, .motion_speed = {20, 50, 80}});
  c.set_fan_speed(95);
  zassert_equal(out.fan, 95);
  zassert_equal(c.settings().motion_speed[0], 95);
  zassert_equal(out.saved.back().motion_speed[0], 95, "stored");
  c.handle(button_event{button_action::plus, 1});
  zassert_equal(out.fan, 100);
  feed(c, activity_level::walking, 2);
  c.handle(button_event{button_action::minus, 2});
  zassert_equal(out.fan, 30);
  zassert_equal(c.fan_speed(), 30);
  zassert_true((c.settings().motion_speed == std::array<std::uint8_t, 3>{100, 30, 80}));
}

ZTEST(controller, test_manual_mode_ignores_activity_and_keeps_its_target) {
  fake_outputs out;
  controller c{out};
  c.set_fan_speed(60);
  feed(c, activity_level::running, 5);
  zassert_equal(out.fan, 60);
  zassert_equal(c.activity_code(), 255);
  zassert_true(out.saved.empty());
  c.update_settings({.mode = fan_mode::motion, .motion_speed = {20, 50, 80}});
  zassert_equal(out.fan, 20);
  c.update_settings({.mode = fan_mode::manual});
  zassert_equal(out.fan, 60);
}

ZTEST(controller, test_battery_warning_once_and_rearmed) {
  fake_outputs out;
  controller c{out};
  auto count = [&] { return std::count(out.signals.begin(), out.signals.end(), signal::battery_warning); };
  c.handle(battery_status{3800, charger_state::not_charging});
  zassert_equal(count(), 0);
  c.handle(battery_status{3600, charger_state::not_charging}); // 10 %.
  zassert_equal(count(), 1);
  c.handle(battery_status{3550, charger_state::not_charging});
  zassert_equal(count(), 1);
  c.handle(battery_status{3700, charger_state::not_charging}); // 30 %: above 15 %, re-armed.
  c.handle(battery_status{3580, charger_state::not_charging});
  zassert_equal(count(), 2);
  zassert_false(out.powered_off);
}

ZTEST(controller, test_battery_warning_respects_stealth) {
  fake_outputs out;
  controller c{out};
  c.configure({.stealth = true});
  c.handle(battery_status{3550, charger_state::not_charging});
  zassert_true(out.signals.empty());
}

ZTEST(controller, test_pwm_pulse_does_not_overflow) {
  static_assert(pwm_pulse_ns(5'000'000, 1000) == 5'000'000);
  static_assert(pwm_pulse_ns(5'000'000, 830) == 4'150'000); // 32-bit math wrapped from 83 %.
  static_assert(pwm_pulse_ns(5'000'000, 0) == 0);
  static_assert(pwm_pulse_ns(1'000'000, 500) == 500'000);
}

ZTEST(controller, test_transport_lock_keeps_the_fan_off_in_motion_mode_and_unlock_restores_it) {
  fake_outputs out;
  controller c{out};
  c.configure({.transport_lock = true, .mode = fan_mode::motion, .motion_speed = {20, 50, 80}});
  c.power_on();
  zassert_equal(out.fan, 0);
  zassert_false(c.settings().follows_motion());
  c.set_strobe(true);
  zassert_false(out.strobe);
  c.handle(button_event{button_action::unlock});
  zassert_equal(out.fan, 20);
  zassert_true(out.strobe, "the strobe target comes back");
  zassert_true(c.settings().follows_motion());
}

ZTEST(controller, test_turning_the_lock_on_stops_a_running_fan_and_strobe) {
  fake_outputs out;
  controller c{out};
  c.power_on();
  c.set_fan_speed(60);
  c.set_strobe(true);
  zassert_equal(out.fan, 60);
  zassert_true(out.strobe);
  c.update_settings({.transport_lock = true});
  zassert_equal(out.fan, 0);
  zassert_false(out.strobe);
  c.update_settings({.transport_lock = false});
  zassert_equal(out.fan, 60);
  zassert_true(out.strobe);
}

ZTEST(controller, test_motion_mode_auto_off_counts_time_at_level_still) {
  fake_outputs out;
  controller c{out};
  c.update_settings({.auto_off_minutes = 1, .mode = fan_mode::motion, .motion_delay_s = 5, .motion_speed = {20, 50, 80}});
  c.power_on();
  zassert_equal(out.fan, 20);
  c.elapsed(50, false);
  c.handle(button_event{button_action::minus, 1}); // A button press restarts the count.
  c.elapsed(50, false);
  zassert_false(out.powered_off);
  c.elapsed(10, true); // So does a phone.
  c.elapsed(50, false);
  zassert_false(out.powered_off);
  feed(c, activity_level::walking, 2); // So does movement.
  c.elapsed(120, false);
  zassert_false(out.powered_off);
  feed(c, activity_level::still, 5);
  c.elapsed(50, false);
  zassert_false(out.powered_off);
  c.elapsed(10, false);
  zassert_true(out.powered_off);
}

ZTEST(controller, test_sensor_lost_runs_the_still_speed_and_reports_still) {
  fake_outputs out;
  controller c{out};
  c.update_settings({.mode = fan_mode::motion, .motion_speed = {20, 50, 80}});
  c.power_on();
  feed(c, activity_level::running, 2);
  zassert_equal(out.fan, 80);
  c.sensor_lost();
  zassert_equal(out.fan, 20);
  zassert_equal(c.activity_code(), 0);
  zassert_equal(out.signals.back(), signal::fault);
}

ZTEST(controller, test_report_respects_stealth) {
  fake_outputs out;
  controller c{out};
  c.report(signal::pairing_refused);
  zassert_equal(out.signals.back(), signal::pairing_refused);
  out.signals.clear();
  c.configure({.stealth = true});
  c.report(signal::fault);
  zassert_true(out.signals.empty());
}

ZTEST(controller, test_low_battery_at_start_powers_off_without_running_the_fan) {
  fake_outputs out;
  controller c{out};
  c.check_start({3399, charger_state::not_charging});
  zassert_true(out.powered_off);
  zassert_equal(out.signals.back(), signal::low_battery);
  zassert_equal(out.fan, 0);

  fake_outputs ok;
  controller d{ok};
  d.check_start({3400, charger_state::not_charging});
  d.check_start({3000, charger_state::charging});
  zassert_false(ok.powered_off);
}

ZTEST(controller, test_settings_changed_is_called_once_per_change) {
  fake_outputs out;
  controller c{out};
  c.configure({.mode = fan_mode::motion});
  c.power_on();
  zassert_equal(out.saved.size(), 0);
  c.handle(button_event{button_action::plus, 1});
  zassert_equal(out.saved.size(), 1);
  c.set_fan_speed(30);
  zassert_equal(out.saved.size(), 2);
  c.update_settings({});
  zassert_equal(out.saved.size(), 3);
}

ZTEST(controller, test_value_and_length_checks) {
  zassert_true(is_fan_percent(0));
  zassert_true(is_fan_percent(100));
  zassert_false(is_fan_percent(101));
  zassert_true(is_strobe_value(1));
  zassert_false(is_strobe_value(2));
  zassert_true(is_settings_size(8) && is_settings_size(6) && is_settings_size(2));
  zassert_false(is_settings_size(0) || is_settings_size(7) || is_settings_size(9));
}
