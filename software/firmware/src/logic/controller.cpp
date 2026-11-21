#include "controller.hpp"

#include <algorithm>

namespace gtfog {

void controller::power_on() {
  out.show(signal::power_on);
  set_fan_speed(0);
  set_strobe(false);
}

void controller::handle(button_event event) {
  switch(event.action) {
  case button_action::plus:
    change_fan_speed(static_cast<int>(event.steps));
    break;
  case button_action::minus:
    change_fan_speed(-static_cast<int>(event.steps));
    break;
  case button_action::toggle_strobe:
    set_strobe(!strobe_on);
    break;
  case button_action::power_off:
    set_strobe(false);
    set_fan_speed(0);
    out.power_off();
    break;
  }
}

void controller::change_fan_speed(int delta) {
  int wanted = static_cast<int>(fan) + delta;
  set_fan_speed(static_cast<unsigned>(std::clamp(wanted, 0, 100)));
  bool at_limit = wanted <= 0 || wanted >= 100;
  out.show(at_limit ? signal::speed_limit : (delta > 0 ? signal::speed_up : signal::speed_down));
}

void controller::set_fan_speed(unsigned percent) {
  fan = std::min(percent, 100u);
  out.set_fan(fan);
}

void controller::set_strobe(bool on) {
  strobe_on = on;
  out.set_strobe(on);
}

void controller::handle(battery_status status) {
  battery = battery_percentage(status.millivolts);
  if(guard.update(status)) {
    out.show(signal::low_battery);
    handle(button_event{button_action::power_off});
  }
}

} // namespace gtfog
