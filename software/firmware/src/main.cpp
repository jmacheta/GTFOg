
#include "charger_status.hpp"
#include "fan.hpp"
#include "runtime_config.hpp"
#include "status_led.hpp"

#include <zephyr/kernel.h>

#include <array>
#include <cstdio>
#include <tuple>
int mainxd();
int nvs_init();
int main(void) {
    auto& status_led = status_light_instance();
    auto& fan        = fan_instance();


    std::array colors = {Colors::Red, Colors::Green, Colors::Blue, Colors::White, Colors::Black};
    int        i      = 0;
    
            auto v = Configuration::get<int>("wtf");
            int  x = (v) ? v.value() : (int)v.error();

            printk("value: %d\n", x);
            ++x;
            Configuration::set("wtf", x);
    mainxd();
    // nvs_init();
    while (1) {
        for (auto color : colors) {
            ++i;
            if (i > 200) {
                i = 0;
            }
            std::ignore = fan.set_speed(i);
            std::ignore = status_led.set_color(color);
            printk("uptime: % " PRId64 " \n", k_uptime_get());

            printf("charger state: %d\n", static_cast<int>(get_charger_status().value_or(ChargerStatus{33})));
            k_msleep(1000);

            auto v = Configuration::get<int>("wtf");
            int  x = (v) ? v.value() : (int)v.error();

            printk("value: %d\n", x);
            ++x;
            Configuration::set("wtf", x);
        }
    }
}