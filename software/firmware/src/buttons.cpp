// Samples the plus and minus buttons and feeds them to the gesture detector.
#include "app.hpp"

#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include <chrono>

LOG_MODULE_REGISTER(buttons, LOG_LEVEL_INF);

namespace {

constexpr auto poll_interval = K_MSEC(20);

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
  if(init_error != 0) {
    LOG_ERR("buttons: %d", init_error);
    // ponytail: no interrupts, so only the polling below could work; show the fault once, after the power-on signal.
    k_sleep(K_SECONDS(2));
    with_controller([](gtfog::controller &c) { c.report(gtfog::signal::fault); });
  }
  while(true) {
    auto now = std::chrono::milliseconds{k_uptime_get()};
    if(auto event = gestures.update(gpio_pin_get_dt(&plus) > 0, gpio_pin_get_dt(&minus) > 0, now)) {
      with_controller([&](gtfog::controller &c) { c.handle(*event); });
    }
    k_sem_take(&button_changed, gestures.is_idle() ? K_FOREVER : poll_interval);
  }
}

K_THREAD_DEFINE(buttons, 1024, buttons_thread, nullptr, nullptr, nullptr, 5, 0, 0);

} // namespace

void buttons_arm_wake() {
  constexpr auto release_timeout_ms = 10'000;
  for(int waited = 0; waited < release_timeout_ms && (gpio_pin_get_dt(&plus) > 0 || gpio_pin_get_dt(&minus) > 0); waited += 10) {
    k_msleep(10);
  }
  gpio_remove_callback_dt(&plus, &plus_callback);
  gpio_remove_callback_dt(&minus, &minus_callback);
  if(gpio_pin_interrupt_configure_dt(&plus, GPIO_INT_LEVEL_ACTIVE) != 0 || gpio_pin_interrupt_configure_dt(&minus, GPIO_INT_LEVEL_ACTIVE) != 0) {
    LOG_ERR("cannot arm the buttons for wake-up");
  }
}
