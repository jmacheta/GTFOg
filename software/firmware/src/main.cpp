
#include "fan.hpp"
#include "status_led.hpp"

#include <zephyr/kernel.h>
#include "charger_status.hpp"
#include <array>
#include <cstdio>
#include <tuple>

int main(void) {
    auto& status_led = status_light_instance();
    auto& fan        = fan_instance();


    std::array colors = {Colors::Red, Colors::Green, Colors::Blue, Colors::White, Colors::Black};

    printk("Hello World! %s\n", CONFIG_BOARD);
    printf("Hello printf!\n");
    int i = 0;
    while (1) {
        for (auto color : colors) {
            ++i;
            if (i > 200) {
                i = 0;
            }
            std::ignore = fan.set_speed(i);
            std::ignore = status_led.set_color(color);
            k_sleep(K_MSEC(1000));

            printk("charger state: %d\n", static_cast<int>(get_charger_status().value_or(ChargerStatus{33})));
        }
    }
}