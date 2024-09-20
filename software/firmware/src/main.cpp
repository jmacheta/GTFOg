
#include "charger_status.hpp"
#include "fan.hpp"
#include "status_led.hpp"

#include <zephyr/kernel.h>

#include <array>
#include <cstdio>
#include <tuple>

int main(void) {
    auto& status_led = status_light_instance();
    auto& fan        = fan_instance();


    std::array colors = {Colors::Red, Colors::Green, Colors::Blue, Colors::White, Colors::Black};

    // printf("Hello World! %s\n", CONFIG_BOARD);
    int i = 0;
    while (1) {
        for (auto color : colors) {
            ++i;
            if (i > 200) {
                i = 0;
            }
            std::ignore = fan.set_speed(i);
            std::ignore = status_led.set_color(color);
            printf("uptime: % " PRId64 " \n", k_uptime_get());

            printf("charger state: %d\n", static_cast<int>(get_charger_status().value_or(ChargerStatus{33})));
            k_msleep(1000);
        }
    }
}