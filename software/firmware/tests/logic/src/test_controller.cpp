#include "controller.hpp"

#include <zephyr/ztest.h>

#include <vector>

using namespace gtfog;

namespace {

struct fake_outputs final : outputs {
  unsigned fan{999};
  bool strobe{true};
  std::vector<signal> signals;
  bool powered_off{false};

  void set_fan(unsigned percent) override { fan = percent; }
  void set_strobe(bool on) override { strobe = on; }
  void show(signal s) override { signals.push_back(s); }
  void power_off() override { powered_off = true; }
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
