
// #include "charger_status.hpp"
// #include "fan.hpp"
// #include "runtime_config.hpp"
#include "status_led.hpp"
// #include "system.hpp"

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/util.h>

#include <array>
#include <cassert>
#include <cstdio>
#include <tuple>


int mainxd();

int main(void) {
    // system_process_event(events::power_on{});


    auto& status_led = status_light_instance();
    // auto& fan        = fan_instance();


    std::array colors = {Colors::Red, Colors::Green, Colors::Blue};


    while (1) {
        for (auto color : colors) {
            std::ignore = status_led.set_color(color);

            k_msleep(100);
        }
    }
}


#include <hal/nrf_power.h>
#include <zephyr/init.h>


static int board_nrf52840dongle_nrf52840_init(void) {
    /* if the nrf52840dongle_nrf52840 board is powered from USB
     * (high voltage mode), GPIO output voltage is set to 1.8 volts by
     * default and that is not enough to turn the green and blue LEDs on.
     * Increase GPIO voltage to 3.0 volts.
     */
    if ((nrf_power_mainregstatus_get(NRF_POWER) == NRF_POWER_MAINREGSTATUS_HIGH) && ((NRF_UICR->REGOUT0 & UICR_REGOUT0_VOUT_Msk) == (UICR_REGOUT0_VOUT_DEFAULT << UICR_REGOUT0_VOUT_Pos))) {
        NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Wen << NVMC_CONFIG_WEN_Pos;
        while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {
            ;
        }

        NRF_UICR->REGOUT0 = (NRF_UICR->REGOUT0 & ~((uint32_t)UICR_REGOUT0_VOUT_Msk)) | (UICR_REGOUT0_VOUT_3V3 << UICR_REGOUT0_VOUT_Pos);

        NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren << NVMC_CONFIG_WEN_Pos;
        while (NRF_NVMC->READY == NVMC_READY_READY_Busy) {
            ;
        }

        /* a reset is required for changes to take effect */
        NVIC_SystemReset();
    }

    return 0;
}

SYS_INIT(board_nrf52840dongle_nrf52840_init, PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
