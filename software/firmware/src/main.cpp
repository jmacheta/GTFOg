#include "app.hpp"

#include <hal/nrf_power.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

#include <cstdint>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

using namespace gtfog;

namespace {

constexpr auto watchdog_start_margin = K_SECONDS(1);

// A new image confirms itself only when Bluetooth works, else MCUboot reverts it on the next reset.
void confirm_image(int ble_err) {
  if(ble_err == 0 && !boot_is_img_confirmed() && boot_write_img_confirmed() != 0) {
    LOG_ERR("cannot confirm the image");
  }
}

} // namespace

int main() {
  // The settings first (stealth decides whether the LED lights up).
  app_settings_load();
  int ble_err = ble_init();
  if(ble_err != 0) {
    LOG_ERR("Bluetooth: %d", ble_err);
  }
  // A cell that only recovered above the cutoff at rest must not run the fan again: power off before it starts.
  if(auto sample = battery_sample()) {
    if(!battery_allows_start(*sample)) {
      confirm_image(ble_err); // The power off below must not revert a good update.
    }
    with_controller([&](gtfog::controller &control) { control.check_start(*sample); }); // Does not return when the battery is low.
  }
  accelerometer_motion_mode(with_controller([](gtfog::controller &control) {
    control.power_on();
    return control.settings().follows_motion();
  }));
  // The battery thread (and with it the watchdog) starts 500 ms after boot, so wait for it before confirming.
  k_sleep(watchdog_start_margin);
  confirm_image(ble_err);
  LOG_INF("started");
  return 0;
}

namespace {

// The regulator of the nRF52833 (VDDH mode) must give 3.3 V to the GPIOs; a UICR write needs a reset, so it happens once per chip.
// Only an erased value (7) is rewritten: bits cannot be set again without an erase, and a failed write would reset forever.
auto set_gpio_voltage() -> int {
  if(nrf_power_mainregstatus_get(NRF_POWER) != NRF_POWER_MAINREGSTATUS_HIGH) {
    return 0; // Normal voltage mode.
  }
  bool supply_is_3v3 = (NRF_UICR->REGOUT0 & UICR_REGOUT0_VOUT_Msk) == (UICR_REGOUT0_VOUT_3V3 << UICR_REGOUT0_VOUT_Pos);
  bool supply_is_default = (NRF_UICR->REGOUT0 & UICR_REGOUT0_VOUT_Msk) == (UICR_REGOUT0_VOUT_DEFAULT << UICR_REGOUT0_VOUT_Pos);
  if(supply_is_3v3 || !supply_is_default) {
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
