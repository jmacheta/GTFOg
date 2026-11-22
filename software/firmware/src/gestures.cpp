#include "gestures.hpp"

#include <algorithm>
#include <tuple>

namespace gtfog {

auto button_gestures::update(bool plus_pressed, bool minus_pressed, std::chrono::milliseconds now) noexcept -> std::optional<button_event> {
  if(!plus_pressed && !minus_pressed) {
    waiting = false;
  }

  if(plus_pressed && minus_pressed && !chord) {
    chord = true;
    plus.stuck = minus.stuck = false;
    chord_since = now;
    chord_done = false;
  }

  if(chord) {
    plus.pressed = plus_pressed;
    minus.pressed = minus_pressed;
    auto held = now - chord_since;
    bool released = !plus_pressed || !minus_pressed;
    bool fire_strobe = released && !chord_done && held >= timing.toggle_strobe && held < timing.power_off;
    bool fire_power_off = !released && !chord_done && held >= timing.power_off;
    chord_done = chord_done || released || fire_power_off;
    chord = plus_pressed || minus_pressed;
    if(fire_strobe) {
      return button_event{button_action::toggle_strobe};
    }
    return fire_power_off ? std::optional{button_event{button_action::power_off}} : std::nullopt;
  }

  if(waiting) {
    return std::nullopt;
  }
  if(auto event = single(plus, plus_pressed, button_action::plus, now)) {
    std::ignore = single(minus, minus_pressed, button_action::minus, now); // Only the plus event is reported; this call keeps minus's state current.
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
    bool counted = b.pressed && held >= timing.short_press && held < timing.stuck;
    bool repeated = b.next_repeat != b.pressed_at + timing.long_press;
    b.pressed = false;
    if(!counted) {
      return std::nullopt;
    }
    if(held < timing.long_press) {
      return action == button_action::plus ? short_plus(now) : button_event{action};
    }
    // Released after the long-press time but before the first repeat: one step.
    return repeated ? std::nullopt : std::optional{button_event{action}};
  }

  if(!b.pressed || b.stuck) {
    return std::nullopt;
  }
  if(held >= timing.stuck) {
    b.stuck = true;
    return std::nullopt;
  }
  if(now < b.next_repeat) {
    return std::nullopt;
  }
  bool later = b.next_repeat != b.pressed_at + timing.long_press;
  b.next_repeat = now + timing.repeat_interval;
  return button_event{action, steps_after(held), later};
}

auto button_gestures::short_plus(std::chrono::milliseconds now) noexcept -> button_event {
  if(taps == 0 || now - first_tap > timing.unlock_window) {
    taps = 0;
    first_tap = now;
  }
  if(++taps < timing.unlock_taps) {
    return {button_action::plus};
  }
  taps = 0;
  return {button_action::unlock};
}

auto button_gestures::steps_after(std::chrono::milliseconds held) const noexcept -> unsigned {
  auto steps = static_cast<unsigned>(held / timing.acceleration) + 1;
  return std::min(steps, timing.max_steps);
}

} // namespace gtfog
