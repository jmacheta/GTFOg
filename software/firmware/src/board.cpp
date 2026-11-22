// The controller's outputs on the GTFOg board: fan and strobe PWM, RGB status LED, System OFF.
#include "app.hpp"

#include <zephyr/drivers/pwm.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/poweroff.h>

#include <cstdint>

LOG_MODULE_REGISTER(board, LOG_LEVEL_INF);

namespace {

using namespace gtfog;

// ponytail: 20 % keeps the blower spinning; tune per fan model.
constexpr unsigned fan_min_duty_permille = 200;
constexpr std::int32_t strobe_half_period_ms = 100;

pwm_dt_spec const fan_pwm = PWM_DT_SPEC_GET(DT_NODELABEL(fan));
pwm_dt_spec const strobe_pwm = PWM_DT_SPEC_GET(DT_NODELABEL(strobe));
pwm_dt_spec const led_pwm[] = {
    PWM_DT_SPEC_GET(DT_NODELABEL(led_r)),
    PWM_DT_SPEC_GET(DT_NODELABEL(led_g)),
    PWM_DT_SPEC_GET(DT_NODELABEL(led_b)),
};

struct color {
  std::uint8_t red, green, blue;
};

constexpr color black{0, 0, 0};
constexpr color green{0, 255, 0};
constexpr color yellow{255, 255, 0};
constexpr color magenta{255, 0, 255};
constexpr color red{255, 0, 0};
constexpr color cyan{0, 255, 255};

struct pattern {
  color shown;
  unsigned blinks;
  std::int32_t on_ms;
  std::int32_t off_ms;
};

constexpr auto pattern_of(signal what) noexcept -> pattern {
  switch(what) {
  case signal::power_on:
    return {green, 3, 50, 50};
  case signal::speed_up:
    return {green, 1, 25, 0};
  case signal::speed_down:
    return {yellow, 1, 25, 0};
  case signal::speed_limit:
    return {magenta, 1, 50, 0};
  case signal::low_battery:
    return {red, 3, 200, 200};
  case signal::locked:
    return {red, 1, 50, 0};
  case signal::unlocked:
    return {green, 2, 100, 100};
  case signal::battery_warning:
    return {red, 2, 100, 100};
  case signal::pairing_refused:
    return {magenta, 2, 100, 100};
  case signal::fault:
    return {red, 4, 50, 50};
  case signal::power_off:
    return {cyan, 1, 300, 0};
  }
  return {black, 0, 0, 0};
}

void set_pwm(pwm_dt_spec const &spec, unsigned permille) {
  // An LED or strobe glitch is harmless, so a failed write is only logged.
  if(int err = pwm_set_pulse_dt(&spec, pwm_pulse_ns(spec.period, permille)); err != 0) {
    LOG_WRN("PWM %u: %d", spec.channel, err);
  }
}

void set_color(color shown) {
  set_pwm(led_pwm[0], shown.red * 1000u / 255u);
  set_pwm(led_pwm[1], shown.green * 1000u / 255u);
  set_pwm(led_pwm[2], shown.blue * 1000u / 255u);
}

// The pattern state is touched by show() (any thread), indicator_step() (system workqueue) and power_off(); `pattern_lock`
// guards it. The PWM writes happen outside the lock.
pattern current_pattern;
unsigned pattern_phase;
std::int64_t phase_end_ms; // Uptime at which the phase shown last ends.
k_spinlock pattern_lock;

void indicator_step(k_work *work);
K_WORK_DELAYABLE_DEFINE(indicator_work, indicator_step);

void indicator_step(k_work *) {
  auto key = k_spin_lock(&pattern_lock);
  if(pattern_phase >= 2 * current_pattern.blinks) {
    k_spin_unlock(&pattern_lock, key);
    set_color(black);
    return;
  }
  auto shown = current_pattern;
  bool lit = pattern_phase++ % 2 == 0;
  auto duration_ms = lit ? shown.on_ms : shown.off_ms;
  phase_end_ms = k_uptime_get() + duration_ms;
  k_spin_unlock(&pattern_lock, key);
  set_color(lit ? shown.shown : black);
  k_work_schedule(&indicator_work, K_MSEC(duration_ms));
}

// `strobe_mutex` guards the two flags and the strobe PWM, so a step that runs while the strobe is switched off cannot undo it.
bool strobe_wanted;
bool strobe_lit;
K_MUTEX_DEFINE(strobe_mutex);

void strobe_step(k_work *work);
K_WORK_DELAYABLE_DEFINE(strobe_work, strobe_step);

void strobe_step(k_work *) {
  k_mutex_lock(&strobe_mutex, K_FOREVER);
  if(strobe_wanted) {
    strobe_lit = !strobe_lit;
    set_pwm(strobe_pwm, strobe_lit ? 1000 : 0);
    k_work_schedule(&strobe_work, K_MSEC(strobe_half_period_ms));
  }
  k_mutex_unlock(&strobe_mutex);
}

class board_outputs final : public outputs {
public:
  void set_fan(unsigned percent) override {
    if(int err = pwm_set_pulse_dt(&fan_pwm, pwm_pulse_ns(fan_pwm.period, fan_duty_permille(percent, fan_min_duty_permille))); err != 0) {
      LOG_ERR("fan PWM: %d", err);
    }
    ble_state_changed();
  }

  void set_strobe(bool on) override {
    k_mutex_lock(&strobe_mutex, K_FOREVER);
    strobe_wanted = on;
    if(on) {
      k_work_schedule(&strobe_work, K_NO_WAIT);
    } else {
      k_work_cancel_delayable(&strobe_work);
      strobe_lit = false;
      set_pwm(strobe_pwm, 0);
    }
    k_mutex_unlock(&strobe_mutex);
    ble_state_changed();
  }

  void show(signal what) override {
    auto key = k_spin_lock(&pattern_lock);
    current_pattern = pattern_of(what);
    pattern_phase = 0;
    phase_end_ms = 0; // The new pattern does not wait for the old one's phase.
    k_spin_unlock(&pattern_lock, key);
    k_work_reschedule(&indicator_work, K_NO_WAIT); // Replaces a step that is waiting for its phase to end.
  }

  void settings_changed(device_settings const &settings) override {
    ble_settings_changed(settings);
    accelerometer_motion_mode(settings.follows_motion());
  }

  void power_off() override {
    LOG_INF("power off");
    // The caller holds the controller mutex, so the watchdog is not fed: 30 s - 10 s since the last feed - 13 s here leaves about 7 s.
    // The system workqueue may be blocked on that mutex, so the pattern finishes here and no work item is awaited.
    k_work_cancel_delayable(&indicator_work);
    auto key = k_spin_lock(&pattern_lock);
    auto shown = current_pattern;
    auto phase = pattern_phase;
    auto end_ms = phase_end_ms;
    pattern_phase = 2 * shown.blinks; // Nothing left for a work item that still runs.
    k_spin_unlock(&pattern_lock, key);
    if(auto left_ms = end_ms - k_uptime_get(); left_ms > 0) {
      k_msleep(left_ms);
    }
    for(; phase < 2 * shown.blinks; ++phase) {
      bool lit = phase % 2 == 0;
      set_color(lit ? shown.shown : black);
      k_msleep(lit ? shown.on_ms : shown.off_ms);
    }
    set_color(black);
    ble_settings_flush(); // In this thread: the system workqueue may be blocked on the controller mutex.
    accelerometer_power_off();
    buttons_arm_wake();
    sys_poweroff();
  }
};

board_outputs board;
gtfog::controller the_controller{board};

} // namespace

namespace gtfog {

K_MUTEX_DEFINE(controller_mutex);

auto app_controller() noexcept -> gtfog::controller & { return the_controller; }

} // namespace gtfog
