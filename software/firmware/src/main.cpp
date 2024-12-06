
#include "events.hpp"
#include "system.hpp"

#include <hal/nrf_power.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/usb/usb_device.h>

#include <tuple>


int main(void) {
    system_process_event(events::request_power_on{});

    std::ignore = usb_enable(NULL);

    while (1) {
        k_msleep(200);
        // }
    }
}

 
static int set_gpio_voltage(void) {
    if (nrf_power_mainregstatus_get(NRF_POWER) != NRF_POWER_MAINREGSTATUS_HIGH) {
        // Low power supply mode, do nothing
        return 0;
    }
    bool supply_is_3v3 = (NRF_UICR->REGOUT0 & UICR_REGOUT0_VOUT_Msk) == (UICR_REGOUT0_VOUT_3V3 << UICR_REGOUT0_VOUT_Pos);
    if (supply_is_3v3) {
        // Already set to 3.3V, do nothing
        return 0;
    }

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

SYS_INIT(set_gpio_voltage, PRE_KERNEL_1, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
