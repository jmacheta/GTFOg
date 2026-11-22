#include "app.hpp"

#include <hal/nrf_power.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

int main() {
  // Bluetooth first: it loads the settings, and stealth mode decides whether power_on() lights the LED.
  int ble_err = ble_init();
  if(ble_err != 0) {
    LOG_ERR("Bluetooth: %d", ble_err);
  }
  accelerometer_motion_mode(with_controller([](gtfog::controller &c) {
    c.power_on();
    return c.settings().mode == gtfog::fan_mode::motion;
  }));
  // A new image runs on trial after an update. It confirms itself only when Bluetooth works, the update path for the
  // next fix; otherwise MCUboot reverts it on the next reset.
  if(ble_err == 0 && !boot_is_img_confirmed() && boot_write_img_confirmed() != 0) {
    LOG_ERR("cannot confirm the image");
  }
  LOG_INF("started");
  return 0;
}

// The nRF52833 runs from VDDH (high voltage mode); its regulator must give 3.3 V to the GPIOs. Writing UICR needs a
// reset, so this happens once, on the first boot of a new chip.
static int set_gpio_voltage(void) {
  if(nrf_power_mainregstatus_get(NRF_POWER) != NRF_POWER_MAINREGSTATUS_HIGH) {
    // Low power supply mode, do nothing
    return 0;
  }
  bool supply_is_3v3 = (NRF_UICR->REGOUT0 & UICR_REGOUT0_VOUT_Msk) == (UICR_REGOUT0_VOUT_3V3 << UICR_REGOUT0_VOUT_Pos);
  if(supply_is_3v3) {
    // Already set to 3.3V, do nothing
    return 0;
  }

  NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen << NVMC_CONFIG_WEN_Pos;
  while(NRF_NVMC->READY == NVMC_READY_READY_Busy) {
    ;
  }

  NRF_UICR->REGOUT0 = (NRF_UICR->REGOUT0 & ~((uint32_t)UICR_REGOUT0_VOUT_Msk)) | (UICR_REGOUT0_VOUT_3V3 << UICR_REGOUT0_VOUT_Pos);

  NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren << NVMC_CONFIG_WEN_Pos;
  while(NRF_NVMC->READY == NVMC_READY_READY_Busy) {
    ;
  }

  NVIC_SystemReset();
}

SYS_INIT(set_gpio_voltage, PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
