#ifndef GTFOG_LOGIC_BUTTONS_HPP
#define GTFOG_LOGIC_BUTTONS_HPP

#include <chrono>
#include <optional>

namespace gtfog {

using namespace std::chrono_literals;

enum class button_action {
  plus,          // Raise the fan speed by `steps`.
  minus,         // Lower the fan speed by `steps`.
  toggle_strobe, // Both buttons released after `button_timing::toggle_strobe`, before `button_timing::power_off`.
  power_off,     // Both buttons held for `button_timing::power_off`.
  unlock,        // Third short press of plus within `button_timing::unlock_window`, instead of `plus`.
};

struct button_event {
  button_action action;
  unsigned steps{1};

  bool operator==(button_event const &) const = default;
};

struct button_timing {
  std::chrono::milliseconds short_press{50ms}; // Shorter presses are bounces.
  std::chrono::milliseconds long_press{500ms}; // Held this long, a button repeats.
  std::chrono::milliseconds repeat_interval{200ms};
  std::chrono::milliseconds acceleration{3s}; // Every this much of holding adds a step per repeat.
  unsigned max_steps{5};
  std::chrono::milliseconds stuck{30s}; // Held this long, a button is ignored until released.
  std::chrono::milliseconds toggle_strobe{2s};
  std::chrono::milliseconds power_off{5s};
  std::chrono::milliseconds unlock_window{2s};
  unsigned unlock_taps{3};
};

// Turns button levels sampled over time into actions. Pure logic: the caller samples the pins and passes the time.
class button_gestures {
public:
  explicit button_gestures(button_timing timing = {}) noexcept : timing(timing) {}

  // Call on every pin change and periodically while a button is pressed; returns at most one action per call.
  [[nodiscard]] auto update(bool plus_pressed, bool minus_pressed, std::chrono::milliseconds now) noexcept -> std::optional<button_event>;

  // Ignores the buttons until both have been seen released once; for a press that is still going on at boot.
  void wait_for_release() noexcept { waiting = true; }

  // True when no button needs a timer (none pressed, or only stuck ones), so the caller can wait for the next pin change.
  [[nodiscard]] auto is_idle() const noexcept -> bool { return (!plus.pressed || plus.stuck) && (!minus.pressed || minus.stuck); }

private:
  struct button {
    bool pressed{false};
    bool stuck{false}; // Held for `button_timing::stuck`; ignored until released.
    std::chrono::milliseconds pressed_at{};
    std::chrono::milliseconds next_repeat{};
  };

  [[nodiscard]] auto single(button &b, bool is_pressed, button_action action, std::chrono::milliseconds now) noexcept -> std::optional<button_event>;
  [[nodiscard]] auto short_plus(std::chrono::milliseconds now) noexcept -> button_event;
  [[nodiscard]] auto steps_after(std::chrono::milliseconds held) const noexcept -> unsigned;

  button_timing timing;
  button plus;
  button minus;
  bool waiting{false}; // See wait_for_release().
  bool chord{false};   // Both buttons went down together; single actions stay off until both are released.
  std::chrono::milliseconds chord_since{};
  bool chord_done{false}; // The chord fired, or a button was let go: it cannot fire any more.
  unsigned taps{0};       // Short presses of plus within the unlock window.
  std::chrono::milliseconds first_tap{};
};

} // namespace gtfog

#endif
