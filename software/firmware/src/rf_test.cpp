// RF carrier test: an unmodulated carrier on a chosen Bluetooth channel for bench measurements, then a cold reboot.
// The SoftDevice Controller is stopped with bt_disable(); MPSL stays initialised but idle and the test drives RADIO directly (as
// the NCS radio_test sample does). mpsl_lib_uninit() is not used: it needs dynamic interrupts and stops the clock the kernel runs on.
#include "app.hpp"

#include <hal/nrf_radio.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <tuple>

LOG_MODULE_REGISTER(rf_test, LOG_LEVEL_INF);

using namespace gtfog;

namespace {

constexpr auto reply_delay = K_MSEC(300); // Lets the write response reach the phone before the link is closed.
constexpr auto poll_interval = K_MSEC(20);
constexpr auto blink_interval_ms = 1000;
constexpr auto ramp_up_timeout_ms = 10;

device const *const clock_device = DEVICE_DT_GET(DT_NODELABEL(clock));

std::atomic<bool> running{false};
rf_test_request pending{};
K_SEM_DEFINE(requested, 0, 1);

// HFXO on, RADIO in BLE 1 Mbit mode at 0 dBm, then TXEN without START: the transmitter idles in TXIDLE and sends the bare carrier.
auto start_carrier(unsigned frequency_mhz) -> int {
  if(int err = clock_control_on(clock_device, CLOCK_CONTROL_NRF_SUBSYS_HF); err != 0) {
    return err;
  }
  nrf_radio_power_set(NRF_RADIO, true);
  nrf_radio_shorts_set(NRF_RADIO, 0);
  nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_BLE_1MBIT);
  nrf_radio_frequency_set(NRF_RADIO, static_cast<std::uint16_t>(frequency_mhz));
  nrf_radio_txpower_set(NRF_RADIO, NRF_RADIO_TXPOWER_0DBM);
  nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_TXEN);
  for(int waited = 0; waited < ramp_up_timeout_ms; ++waited) {
    if(nrf_radio_state_get(NRF_RADIO) == NRF_RADIO_STATE_TXIDLE) {
      return 0;
    }
    k_msleep(1);
  }
  return nrf_radio_state_get(NRF_RADIO) == NRF_RADIO_STATE_TXIDLE ? 0 : -EIO;
}

void rf_test_thread(void *, void *, void *) {
  k_sem_take(&requested, K_FOREVER);
  auto const request = pending;
  k_sleep(reply_delay);
  ble_settings_flush(); // No flash write is possible while the radio belongs to the test.
  board_rf_test_tick();
  int err = ble_stop();
  err = err ?: start_carrier(rf_test_frequency_mhz(request.channel));
  if(err != 0) {
    LOG_ERR("rf test: %d", err);
  }
  // The battery thread keeps feeding the watchdog; a failed start only waits for the first tick below to end the test.
  auto const end_ms = k_uptime_get() + std::chrono::milliseconds{request.duration}.count();
  // A button that is still down from the start of the test does not end it before the first second.
  for(int elapsed_ms = 0; err == 0 && k_uptime_get() < end_ms && !(elapsed_ms > blink_interval_ms && buttons_pressed()); elapsed_ms += 20) {
    if(elapsed_ms % blink_interval_ms == 0) {
      board_rf_test_tick();
    }
    k_sleep(poll_interval);
  }
  sys_reboot(SYS_REBOOT_COLD);
}

K_THREAD_DEFINE(rf_test, 2048, rf_test_thread, nullptr, nullptr, nullptr, 6, 0, 0);

} // namespace

auto gtfog::rf_test_start(rf_test_request const &request) -> int {
  if(running.exchange(true)) {
    return -EBUSY;
  }
  pending = request;
  k_sem_give(&requested);
  return 0;
}

auto gtfog::rf_test_running() -> bool { return running; }
