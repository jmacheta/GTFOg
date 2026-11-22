#ifndef GTFOG_CONTROLLER_HPP
#define GTFOG_CONTROLLER_HPP

#include "battery_model.hpp"
#include "gestures.hpp"
#include "motion.hpp"

#include <boost/sml.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>

namespace gtfog {

// What the user sees on the status LED; the board layer picks the colors.
enum class signal {
  power_on,
  speed_up,
  speed_down,
  speed_limit,
  low_battery,
  locked,          // A button did nothing because of the transport lock.
  unlocked,        // The unlock gesture cleared the transport lock.
  battery_warning, // The battery is nearly empty; the device keeps running.
  pairing_refused, // A new phone tried to pair after the pairing window closed.
  fault,           // A part (buttons, ADC, accelerometer) failed for good; the device keeps running without it.
  power_off,       // Last, right before power_off(); the board waits for it to finish.
  rf_test,         // The RF carrier test is on; only the test shows it, once a second, whatever the stealth setting.
};

enum class fan_mode : std::uint8_t {
  manual = 0, // The fan runs at the speed the user set.
  motion = 1, // The fan speed follows the activity: one target per `activity_level`.
};

// User settings, kept in flash and written over Bluetooth.
struct device_settings {
  bool stealth{false};              // The status LED never lights up.
  bool transport_lock{false};       // Buttons only wake the device and power it off; fan and strobe stay off.
  std::uint8_t auto_off_minutes{0}; // Power off after this long idle (fan and strobe off, no phone); 0 never. While locked: 1 minute.
  std::uint8_t fan_step{5};         // Fan change per button click, in %, 1..25.
  fan_mode mode{fan_mode::manual};
  std::uint8_t motion_delay_s{60};                       // Motion mode: seconds of lower activity before the fan slows down, 5..255.
  std::array<std::uint8_t, 3> motion_speed{20, 50, 100}; // Motion mode: fan speed in % while still, walking, running, 0..100.

  // The accelerometer is only needed while the fan follows it; the transport lock keeps the fan off.
  [[nodiscard]] constexpr auto follows_motion() const noexcept -> bool { return mode == fan_mode::motion && !transport_lock; }

  friend auto operator==(device_settings const &, device_settings const &) -> bool = default;
};

// Wire and flash format: flags (bit 0 stealth, bit 1 transport lock), auto-off minutes, fan step, fan mode, motion delay in
// seconds, still, walking and running speed. Shorter 6- and 2-byte forms are accepted; fields they do not carry keep the
// values of `base`.
constexpr std::size_t settings_size = 8;
constexpr std::size_t settings_size_v2 = 6;
constexpr std::size_t settings_size_v1 = 2;
[[nodiscard]] constexpr auto is_settings_size(std::size_t len) noexcept -> bool {
  return len == settings_size || len == settings_size_v2 || len == settings_size_v1;
}
// Values a phone may write to the fan and strobe characteristics.
[[nodiscard]] constexpr auto is_fan_percent(unsigned value) noexcept -> bool { return value <= 100; }
[[nodiscard]] constexpr auto is_strobe_value(unsigned value) noexcept -> bool { return value <= 1; }
[[nodiscard]] auto encode(device_settings settings) noexcept -> std::array<std::uint8_t, settings_size>;
[[nodiscard]] auto decode_settings(std::span<std::uint8_t const> bytes, device_settings base = {}) noexcept -> std::optional<device_settings>;

// The hardware the controller drives. The firmware implements it on the board, the tests with a fake.
class outputs {
public:
  virtual void set_fan(unsigned percent) = 0; // 0 stops the fan, 1..100 runs it.
  virtual void set_strobe(bool on) = 0;
  virtual void show(signal shown) = 0;
  virtual void power_off() = 0;                                       // Does not return on the board.
  virtual void settings_changed(device_settings const &settings) = 0; // After the user changed them; to be stored. Must not block.

protected:
  ~outputs() = default;
};

// Maps a fan speed of 0..100 % to a PWM duty cycle of 0..1000 permille. Any speed above 0 gets at least `min_duty`
// so the fan does not stall.
[[nodiscard]] constexpr auto fan_duty_permille(unsigned percent, unsigned min_duty) noexcept -> unsigned {
  if(percent == 0) {
    return 0;
  }
  if(percent > 100) {
    percent = 100;
  }
  return min_duty + (1000 - min_duty) * (percent - 1) / 99;
}

// Pulse width in ns of a PWM period of `period_ns` at `permille` / 1000 duty; 64-bit, as 5 ms x 1000 overflows 32 bits.
[[nodiscard]] constexpr auto pwm_pulse_ns(std::uint32_t period_ns, unsigned permille) noexcept -> std::uint32_t {
  return static_cast<std::uint32_t>(std::uint64_t{period_ns} * permille / 1000);
}

// Everything the controller remembers apart from its states. Its methods are the small steps the transition table calls.
struct controller_data {
  explicit controller_data(outputs &out) noexcept : out(out) {}

  void start();
  void shut_down(signal shown);
  void indicate(signal shown);
  void apply_fan() { out.set_fan(fan_speed()); }
  void apply_strobe() { out.set_strobe(strobe()); }
  void change_fan_speed(int delta);
  void set_fan_speed(unsigned percent);
  void set_strobe(bool on);
  void track_activity(activity_level raw);
  void lose_sensor();
  void store_settings(device_settings const &value);
  void publish_settings();
  void unlock_by_gesture();
  void reset_activity();
  void record_battery(battery_status status); // Before the battery_sample event, so the guards are pure.
  [[nodiscard]] auto idle_limit() const noexcept -> std::chrono::seconds;
  [[nodiscard]] auto in_use(bool phone_connected) const noexcept -> bool;

  [[nodiscard]] auto target_fan() const noexcept -> unsigned {
    return config.mode == fan_mode::motion ? config.motion_speed[static_cast<unsigned>(level)] : fan;
  }
  [[nodiscard]] auto fan_speed() const noexcept -> unsigned { return config.transport_lock ? 0 : target_fan(); }
  [[nodiscard]] auto strobe() const noexcept -> bool { return strobe_on && !config.transport_lock; }

  outputs &out;
  unsigned fan{0}; // The manual mode target.
  activity_level level{activity_level::still};
  activity_level rising_to{activity_level::still};  // Lowest level seen while the raw level stayed above `level`.
  unsigned rising_s{0};                             // Consecutive seconds above `level`; one `track_activity` call per second.
  activity_level falling_to{activity_level::still}; // Highest level seen while the raw level stayed below `level`.
  unsigned falling_s{0};                            // Consecutive seconds below `level`.
  bool strobe_on{false};
  std::uint8_t battery{0};
  // The settings. `transport_lock` and `mode` are a cache of the lock and fan mode region states for the methods above, written
  // only by the entry actions of those states; `controller::settings()` reads the states.
  device_settings config;
  std::chrono::seconds idle{0};
  bool battery_warned{false};
  unsigned low_battery_samples{0}; // Consecutive samples at or below `warning_percent`.
  bool exhausted{false};           // The cutoff guard's verdict on the last sample.
  battery_guard guard;
};

namespace events {
struct power_on {};
struct start_check {
  battery_status status;
}; // The battery sample taken at boot.
struct button {
  button_event press;
};
struct fan_request {
  unsigned percent;
}; // Bluetooth.
struct strobe_request {
  bool on;
}; // Bluetooth.
struct battery_sample {}; // The reading is recorded in `controller_data` first.
struct elapsed {
  std::chrono::seconds passed;
  bool phone_connected;
};
struct activity_sample {
  activity_level raw;
}; // One second of accelerometer data.
struct sensor_lost {};
struct battery_unreadable {}; // The voltage cannot be read for a long time; there is no cutoff.
struct report {
  signal shown;
}; // The board layer's own LED signals.
struct settings_received {
  device_settings value;
  bool user_written;
}; // Loaded from flash, or written over Bluetooth.
} // namespace events

namespace states {
inline constexpr auto running = boost::sml::state<class running_state>;
inline constexpr auto powered_off = boost::sml::state<class powered_off_state>;
inline constexpr auto unlocked = boost::sml::state<class unlocked_state>;
inline constexpr auto locked = boost::sml::state<class locked_state>;
inline constexpr auto manual = boost::sml::state<class manual_state>;
inline constexpr auto motion = boost::sml::state<class motion_state>;
inline constexpr auto synced = boost::sml::state<class synced_state>;
} // namespace states

// ponytail: percentages come from the rested-cell curve, so a fan under load reads lower; measure under load and tune.
constexpr std::uint8_t warning_percent = 10;
constexpr std::uint8_t recovered_percent = 15;
constexpr unsigned warning_samples = 3; // Consecutive samples (30 s), like the cutoff, so fan inrush does not warn.

namespace guards {
using events::battery_sample, events::button, events::elapsed, events::settings_received, events::start_check;

inline constexpr auto pressed_power_off = [](button const &event) { return event.press.action == button_action::power_off; };
inline constexpr auto pressed_plus = [](button const &event) { return event.press.action == button_action::plus; };
inline constexpr auto pressed_minus = [](button const &event) { return event.press.action == button_action::minus; };
inline constexpr auto pressed_strobe = [](button const &event) { return event.press.action == button_action::toggle_strobe; };
inline constexpr auto pressed_unlock = [](button const &event) { return event.press.action == button_action::unlock; };
// Every other press while locked; a held button shows the signal once.
inline constexpr auto refused_while_locked = [](button const &event) {
  return !event.press.repeat && event.press.action != button_action::unlock && event.press.action != button_action::power_off;
};
inline constexpr auto sets_lock = [](settings_received const &event) { return event.value.transport_lock; };
inline constexpr auto clears_lock = [](settings_received const &event) { return !event.value.transport_lock; };
inline constexpr auto sets_motion = [](settings_received const &event) { return event.value.mode == fan_mode::motion; };
inline constexpr auto sets_manual = [](settings_received const &event) { return event.value.mode == fan_mode::manual; };
inline constexpr auto written_by_user = [](settings_received const &event) { return event.user_written; };

inline constexpr auto battery_too_low_to_start = [](start_check const &event) { return !battery_allows_start(event.status); };
inline constexpr auto battery_exhausted = [](controller_data const &data) { return data.exhausted; };
inline constexpr auto battery_low_unwarned = [](controller_data const &data) {
  return data.low_battery_samples >= warning_samples && !data.battery_warned;
};
inline constexpr auto battery_recovered = [](controller_data const &data) { return data.battery > recovered_percent; };

inline constexpr auto in_use = [](controller_data const &data, elapsed const &event) { return data.in_use(event.phone_connected); };
inline constexpr auto idle_limit_reached = [](controller_data const &data, elapsed const &event) {
  auto limit = data.idle_limit();
  return limit != std::chrono::seconds{0} && data.idle + event.passed >= limit;
};

// The activity region cannot see the lock region's state, so it reads the cache.
inline constexpr auto not_locked = [](controller_data const &data) { return !data.config.transport_lock; };
} // namespace guards

namespace actions {
using events::activity_sample, events::battery_sample, events::button, events::elapsed, events::fan_request, events::report,
    events::settings_received, events::strobe_request;

inline constexpr auto start = [](controller_data &data) { data.start(); };
inline constexpr auto shut_down_by_user = [](controller_data &data) { data.shut_down(signal::power_off); };
inline constexpr auto shut_down_low_battery = [](controller_data &data) { data.shut_down(signal::low_battery); };
inline constexpr auto reset_idle = [](controller_data &data) { data.idle = std::chrono::seconds{0}; };
inline constexpr auto count_idle = [](controller_data &data, elapsed const &event) { data.idle += event.passed; };
inline constexpr auto warn_battery = [](controller_data &data) {
  data.battery_warned = true;
  data.indicate(signal::battery_warning);
};
inline constexpr auto clear_battery_warning = [](controller_data &data) { data.battery_warned = false; };
inline constexpr auto lose_sensor = [](controller_data &data) { data.lose_sensor(); };
inline constexpr auto show = [](controller_data &data, report const &event) { data.indicate(event.shown); };
inline constexpr auto set_fan = [](controller_data &data, fan_request const &event) { data.set_fan_speed(event.percent); };
inline constexpr auto set_strobe = [](controller_data &data, strobe_request const &event) { data.set_strobe(event.on); };
inline constexpr auto store_settings = [](controller_data &data, settings_received const &event) { data.store_settings(event.value); };
inline constexpr auto publish_settings = [](controller_data &data) { data.publish_settings(); };
inline constexpr auto track_activity = [](controller_data &data, activity_sample const &event) { data.track_activity(event.raw); };

inline constexpr auto raise_fan = [](controller_data &data, button const &event) {
  data.change_fan_speed(static_cast<int>(event.press.steps * data.config.fan_step));
};
inline constexpr auto lower_fan = [](controller_data &data, button const &event) {
  data.change_fan_speed(-static_cast<int>(event.press.steps * data.config.fan_step));
};
inline constexpr auto raise_fan_one_step = [](controller_data &data) { data.change_fan_speed(data.config.fan_step); };
inline constexpr auto toggle_strobe = [](controller_data &data) { data.set_strobe(!data.strobe_on); };
inline constexpr auto show_locked = [](controller_data &data) { data.indicate(signal::locked); };

// Entry actions: they run after the state has changed, so the effects see the new lock and mode.
inline constexpr auto follow_lock = [](controller_data &data, settings_received const &event) {
  data.config.transport_lock = event.value.transport_lock;
  data.reset_activity(); // The accelerometer was off, so the old level says nothing.
};
inline constexpr auto follow_mode = [](controller_data &data, settings_received const &event) {
  data.config.mode = event.value.mode;
  data.reset_activity();
};
inline constexpr auto unlock_by_gesture = [](controller_data &data) { data.unlock_by_gesture(); };
} // namespace actions

// Five orthogonal regions that see every event: power (first matching row wins), lock, fan mode (with activity tracking) and the
// settings write, published last. The states are authoritative for lock and mode; `config` holds a cache written only by their entry actions.
struct controller_table {
  auto operator()() const {
    using namespace boost::sml;
    using namespace events;
    using namespace states;
    using namespace guards;
    using namespace actions;

    // clang-format off
    return make_transition_table(
      // Power
      *running + event<power_on>                                    / start,
       running + event<start_check>    [battery_too_low_to_start]   / shut_down_low_battery = powered_off,
       running + event<button>         [pressed_power_off]          / shut_down_by_user      = powered_off,
       running + event<button>                                      / reset_idle,
       running + event<battery_sample> [battery_exhausted]          / shut_down_low_battery  = powered_off,
       running + event<battery_sample> [battery_low_unwarned]       / warn_battery,
       running + event<battery_sample> [battery_recovered]          / clear_battery_warning,
       running + event<elapsed>        [in_use]                     / reset_idle,
       running + event<elapsed>        [idle_limit_reached]         / shut_down_by_user      = powered_off,
       running + event<elapsed>                                     / count_idle,
       running + event<sensor_lost>                                 / lose_sensor,
       running + event<battery_unreadable>                          / shut_down_low_battery  = powered_off,
       running + event<report>                                      / show,
       running + event<fan_request>                                 / set_fan,
       running + event<strobe_request>                              / set_strobe,
       running + event<settings_received>                           / store_settings,

      // Transport lock
      *unlocked + event<button>            [pressed_plus]         / raise_fan,
       unlocked + event<button>            [pressed_minus]        / lower_fan,
       unlocked + event<button>            [pressed_strobe]       / toggle_strobe,
       unlocked + event<button>            [pressed_unlock]       / raise_fan_one_step,
       unlocked + event<settings_received> [sets_lock]                                  = locked,
       locked   + event<button>            [pressed_unlock]                             = unlocked, // Entry: restore, signal.
       locked   + event<button>            [refused_while_locked] / show_locked,
       locked   + event<settings_received> [clears_lock]                                = unlocked,
       locked   + on_entry<settings_received>             / follow_lock,
       unlocked + on_entry<settings_received>             / follow_lock,
       unlocked + on_entry<button>                        / unlock_by_gesture,

      // Fan mode
      *manual + event<settings_received>   [sets_motion]                                = motion,
       motion + event<settings_received>   [sets_manual]                                = manual,
       motion + event<activity_sample>     [not_locked]           / track_activity,
       manual + on_entry<settings_received>               / follow_mode,
       motion + on_entry<settings_received>               / follow_mode,

      // Settings write: fan, strobe and storage, once both states above have changed
      *synced + event<settings_received>   [written_by_user]      / publish_settings);
    // clang-format on
  }
};

// The device behavior: buttons, Bluetooth requests and battery readings in, fan, strobe and LED out. A thin wrapper that turns
// calls into events of `controller_table`; not thread safe, the board layer calls it under a mutex.
class controller {
public:
  explicit controller(outputs &out) noexcept : data(out), machine(data) {}

  void power_on() { machine.process_event(events::power_on{}); }
  void handle(button_event event) { machine.process_event(events::button{event}); }
  void set_fan_speed(unsigned percent) { machine.process_event(events::fan_request{percent}); }
  void set_strobe(bool on) { machine.process_event(events::strobe_request{on}); }
  void handle(battery_status status) {
    data.record_battery(status);
    machine.process_event(events::battery_sample{});
  }
  // Settings loaded from flash.
  void configure(device_settings loaded) { machine.process_event(events::settings_received{loaded, false}); }
  // The battery sample taken at boot: powers off at once, without starting the fan, when it is too low to run.
  void check_start(battery_status status) { machine.process_event(events::start_check{status}); }
  // The status LED shows `shown` (unless stealth); for the board layer's own events.
  void report(signal shown) { machine.process_event(events::report{shown}); }
  // The battery voltage cannot be read for a long time: powers off, as the cutoff cannot work.
  void battery_unreadable() { machine.process_event(events::battery_unreadable{}); }
  // The accelerometer does not answer any more: motion mode runs at the still speed.
  void sensor_lost() { machine.process_event(events::sensor_lost{}); }
  // Settings written by the user (Bluetooth): stored through `outputs::settings_changed`.
  void update_settings(device_settings written) { machine.process_event(events::settings_received{written, true}); }
  // Activity measured over one second of accelerometer data (call once per second); only used in motion mode.
  void observe(activity_level raw) { machine.process_event(events::activity_sample{raw}); }
  // Counts idle time for the auto-off; call it periodically with the time since the last call.
  void elapsed(std::chrono::seconds passed, bool phone_connected) { machine.process_event(events::elapsed{passed, phone_connected}); }

  // What the fan runs at: the manual target, or in motion mode the target of the current activity level; 0 while the
  // transport lock is on (the targets are kept).
  [[nodiscard]] auto fan_speed() const noexcept -> unsigned { return data.fan_speed(); }
  // The current activity level for Bluetooth: 0 still, 1 walking, 2 running, 255 in manual mode.
  [[nodiscard]] auto activity_code() const noexcept -> std::uint8_t {
    return data.config.mode == fan_mode::motion ? static_cast<std::uint8_t>(data.level) : 255;
  }
  [[nodiscard]] auto strobe() const noexcept -> bool { return data.strobe(); }
  [[nodiscard]] auto battery_level() const noexcept -> std::uint8_t { return data.battery; }
  // The settings with the lock and the mode taken from the states.
  [[nodiscard]] auto settings() const -> device_settings {
    auto result = data.config;
    result.transport_lock = machine.is(states::locked);
    result.mode = machine.is(states::motion) ? fan_mode::motion : fan_mode::manual;
    return result;
  }

  // Whether the machine is in `state` of `gtfog::states` (a state of any region).
  template <typename State> [[nodiscard]] auto is(State state) const -> bool { return machine.is(state); }

private:
  controller_data data;
  mutable boost::sml::sm<controller_table> machine; // `is()` is not const in SML.
};

} // namespace gtfog

#endif
