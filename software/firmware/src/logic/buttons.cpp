#include "buttons.hpp"

#include <algorithm>
#include <tuple>

namespace gtfog {

auto button_gestures::update(bool plus_pressed, bool minus_pressed, std::chrono::milliseconds now) noexcept -> std::optional<button_event> {
  if(plus_pressed && minus_pressed && !chord) {
    chord = true;
    chord_since = now;
    strobe_sent = false;
    power_off_sent = false;
  }

  if(chord) {
    plus.pressed = plus_pressed;
    minus.pressed = minus_pressed;
    if(!plus_pressed && !minus_pressed) {
      chord = false;
      return std::nullopt;
    }
    if(!plus_pressed || !minus_pressed) {
      strobe_sent = power_off_sent = true; // A button was let go: the chord cannot fire any more.
      return std::nullopt;
    }
    auto held = now - chord_since;
    if(!power_off_sent && held >= timing.power_off) {
      power_off_sent = true;
      return button_event{button_action::power_off};
    }
    if(!strobe_sent && held >= timing.toggle_strobe) {
      strobe_sent = true;
      return button_event{button_action::toggle_strobe};
    }
    return std::nullopt;
  }

  if(auto event = single(plus, plus_pressed, button_action::plus, now)) {
    std::ignore = single(minus, minus_pressed, button_action::minus, now);
    return event;
  }
  return single(minus, minus_pressed, button_action::minus, now);
}

auto button_gestures::single(button &b, bool is_pressed, button_action action, std::chrono::milliseconds now) noexcept
    -> std::optional<button_event> {
  if(is_pressed && !b.pressed) {
    b = {.pressed = true, .pressed_at = now, .next_repeat = now + timing.long_press};
    return std::nullopt;
  }

  auto held = now - b.pressed_at;
  if(!is_pressed) {
    bool was_short = b.pressed && held >= timing.short_press && held < timing.long_press;
    b.pressed = false;
    return was_short ? std::optional{button_event{action}} : std::nullopt;
  }

  if(!b.pressed || held >= timing.stuck || now < b.next_repeat) {
    return std::nullopt;
  }
  b.next_repeat = now + timing.repeat_interval;
  return button_event{action, steps_after(held)};
}

auto button_gestures::steps_after(std::chrono::milliseconds held) const noexcept -> unsigned {
  auto steps = static_cast<unsigned>(held / timing.acceleration) + 1;
  return std::min(steps, timing.max_steps);
}

} // namespace gtfog
