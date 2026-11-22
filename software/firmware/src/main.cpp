#include "app.hpp"

#include <hal/nrf_power.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include <cstdint>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

using namespace gtfog;

int main() {
  // The settings first (stealth decides whether the LED lights up).
  app_settings_load();
  int ble_err = ble_init();
  if(ble_err != 0) {
    LOG_ERR("Bluetooth: %d", ble_err);
  }
  // A cell that only recovered above the cutoff at rest must not run the fan again: power off before it starts.
  if(auto sample = battery_sample()) {
    with_controller([&](gtfog::controller &control) { control.check_start(*sample); }); // Does not return when the battery is low.
  }
  accelerometer_motion_mode(with_controller([](gtfog::controller &control) {
    control.power_on();
    return control.settings().follows_motion();
  }));
  // The battery thread (and with it the watchdog) starts 500 ms after boot, so wait for it before confirming; a new image
  // confirms itself only when Bluetooth works, else MCUboot reverts it on the next reset.
  k_sleep(K_SECONDS(1));
  if(ble_err == 0 && !boot_is_img_confirmed() && boot_write_img_confirmed() != 0) {
    LOG_ERR("cannot confirm the image");
  }
  LOG_INF("started");
  return 0;
}

namespace {

// The nRF52833 runs from VDDH (high voltage mode); its regulator must give 3.3 V to the GPIOs. Writing UICR needs a
// reset, so this happens once, on the first boot of a new chip.
auto set_gpio_voltage() -> int {
  if(nrf_power_mainregstatus_get(NRF_POWER) != NRF_POWER_MAINREGSTATUS_HIGH) {
    return 0; // Normal voltage mode.
  }
  bool supply_is_3v3 = (NRF_UICR->REGOUT0 & UICR_REGOUT0_VOUT_Msk) == (UICR_REGOUT0_VOUT_3V3 << UICR_REGOUT0_VOUT_Pos);
  if(supply_is_3v3) {
    return 0;
  }

  // ponytail: the NVMC ready waits have no timeout; a UICR write takes microseconds and the reset below ends this path.
  NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen << NVMC_CONFIG_WEN_Pos;
  while(NRF_NVMC->READY == NVMC_READY_READY_Busy) {
  }

  NRF_UICR->REGOUT0 = (NRF_UICR->REGOUT0 & ~static_cast<std::uint32_t>(UICR_REGOUT0_VOUT_Msk)) | (UICR_REGOUT0_VOUT_3V3 << UICR_REGOUT0_VOUT_Pos);

  NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren << NVMC_CONFIG_WEN_Pos;
  while(NRF_NVMC->READY == NVMC_READY_READY_Busy) {
  }

  NVIC_SystemReset();
}

SYS_INIT(set_gpio_voltage, PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

} // namespace
