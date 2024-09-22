
#include "charger_status.hpp"
#include "fan.hpp"
#include "runtime_config.hpp"
#include "status_led.hpp"
#include "system.hpp"

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
    system_process_event(events::power_on{});

    auto& status_led = status_light_instance();
    auto& fan        = fan_instance();


    std::array colors = {Colors::Red, Colors::Green, Colors::Blue};


    while (1) {
        for (auto color : colors) {
            std::ignore = status_led.set_color(color);

            k_msleep(100);
        }
    }
}
