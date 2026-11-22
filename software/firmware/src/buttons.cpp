// Samples the plus and minus buttons and feeds them to the gesture detector.
#include "app.hpp"

#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include <chrono>
#include <tuple>

LOG_MODULE_REGISTER(buttons, LOG_LEVEL_INF);

using gtfog::with_controller;

namespace {

constexpr auto poll_interval = K_MSEC(20);
constexpr auto fault_signal_delay = K_SECONDS(2); // Lets the power-on signal finish first.

gpio_dt_spec const plus = GPIO_DT_SPEC_GET(DT_NODELABEL(button_plus), gpios);
gpio_dt_spec const minus = GPIO_DT_SPEC_GET(DT_NODELABEL(button_minus), gpios);

gpio_callback plus_callback;
gpio_callback minus_callback;

K_SEM_DEFINE(button_changed, 0, 1);

void on_change(device const *, gpio_callback *, std::uint32_t) { k_sem_give(&button_changed); }

auto setup(gpio_dt_spec const &button, gpio_callback &callback) -> int {
  if(!gpio_is_ready_dt(&button)) {
    return -ENODEV;
  }
  if(int err = gpio_pin_configure_dt(&button, GPIO_INPUT); err != 0) {
    return err;
  }
  if(int err = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_BOTH); err != 0) {
    return err;
  }
  gpio_init_callback(&callback, on_change, BIT(button.pin));
  return gpio_add_callback_dt(&button, &callback);
}

int init_error = 0;

auto buttons_init() -> int {
  init_error = setup(plus, plus_callback);
  init_error = init_error ?: setup(minus, minus_callback);
  return init_error;
}

SYS_INIT(buttons_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

void buttons_thread(void *, void *, void *) {
  gtfog::button_gestures gestures;
  gestures.wait_for_release(); // The press that woke the device may still be going on.
  if(init_error != 0) {
    LOG_ERR("buttons: %d", init_error);
    // ponytail: no interrupts, so only the polling below could work; show the fault once, after the power-on signal.
    k_sleep(fault_signal_delay);
    with_controller([](gtfog::controller &control) { control.report(gtfog::signal::fault); });
  }
  while(true) {
    auto now = std::chrono::milliseconds{k_uptime_get()};
    if(auto event = gestures.update(gpio_pin_get_dt(&plus) > 0, gpio_pin_get_dt(&minus) > 0, now)) {
      with_controller([&](gtfog::controller &control) { control.handle(*event); });
    }
    k_sem_take(&button_changed, (init_error != 0 || !gestures.is_idle()) ? poll_interval : K_FOREVER);
  }
}

K_THREAD_DEFINE(buttons, 2048, buttons_thread, nullptr, nullptr, nullptr, 5, 0, 0);

} // namespace

void gtfog::buttons_arm_wake() {
  constexpr auto release_timeout_ms = 10'000;
  for(int waited = 0; waited < release_timeout_ms && (gpio_pin_get_dt(&plus) > 0 || gpio_pin_get_dt(&minus) > 0); waited += 10) {
    k_msleep(10);
  }
  // A button still held would wake the chip at once, so a stuck one is left out; with both held there is no choice.
  bool plus_held = gpio_pin_get_dt(&plus) > 0;
  bool minus_held = gpio_pin_get_dt(&minus) > 0;
  auto arm = [](gpio_dt_spec const &button, bool on) {
    return gpio_pin_interrupt_configure_dt(&button, on ? GPIO_INT_LEVEL_ACTIVE : GPIO_INT_DISABLE);
  };
  // Not registered when setup failed (-EINVAL); the level interrupts below do not need the callbacks.
  std::ignore = gpio_remove_callback_dt(&plus, &plus_callback);
  std::ignore = gpio_remove_callback_dt(&minus, &minus_callback);
  if(arm(plus, !plus_held || minus_held) != 0 || arm(minus, !minus_held || plus_held) != 0) {
    LOG_ERR("cannot arm the buttons for wake-up");
  }
}
