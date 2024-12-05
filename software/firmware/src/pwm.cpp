#include "fan.hpp"
#include "include/compile_time_config.hpp"
#include "indicator.hpp"
#include "strobe.hpp"

#include <fmt/compile.h>
#include <fmt/core.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/kernel.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <tuple>


using namespace fmt::literals;


void Indicator::set_color(Color color) {
    static pwm_dt_spec const led_r = PWM_DT_SPEC_GET(DT_NODELABEL(led_r));
    static pwm_dt_spec const led_g = PWM_DT_SPEC_GET(DT_NODELABEL(led_g));
    static pwm_dt_spec const led_b = PWM_DT_SPEC_GET(DT_NODELABEL(led_b));


    if (!is_output_allowed()) {
        color = Colors::Black;
    }

    auto red_pulse   = color.red * led_r.period / UINT8_MAX;
    auto green_pulse = color.green * led_g.period / UINT8_MAX;
    auto blue_pulse  = color.blue * led_b.period / UINT8_MAX;

    // This is absolutely non critical code, so we can ignore the return value.
    std::ignore = pwm_set_dt(&led_r, led_r.period, red_pulse);
    std::ignore = pwm_set_dt(&led_g, led_g.period, green_pulse);
    std::ignore = pwm_set_dt(&led_b, led_b.period, blue_pulse);
}

void Indicator::allow_output(bool allowed) {
    output_allowed = allowed;
    if (!allowed) {
        off();
    }
}

Indicator& indicator_instance() noexcept {
    static Indicator indicator_instance;
    return indicator_instance;
}


void Fan::set_limits(std::uint8_t min, std::uint8_t max) {
    if ((min > max) || (0U == max)) {
        throw std::invalid_argument{"Fan limits"};
    }
    min_speed = min;
    max_speed = max;
}


void Fan::set_speed(uint8_t value) {
    static pwm_dt_spec const fan = PWM_DT_SPEC_GET(DT_NODELABEL(fan));

    if (value != 0) {
        value = std::clamp(value, std::uint8_t{min_speed}, std::uint8_t{max_speed});
    }
    auto fan_pulse = value * fan.period / UINT8_MAX;
    auto result    = pwm_set_dt(&fan, fan.period, fan_pulse);

    if (0 != result) {
        throw std::runtime_error{fmt::format("Fan PWM set: {}"_cf, result)};
    }

    speed = value;
}


Fan& fan_instance() noexcept {
    static Fan fan;
    return fan;
}


int strobe_thread(void);
K_THREAD_DEFINE(strobe, config::strobe_thread_stack_size, strobe_thread, nullptr, nullptr, nullptr, config::strobe_thread_priority, 0, 0);
static pwm_dt_spec const strobe_pwm = PWM_DT_SPEC_GET(DT_NODELABEL(strobe));


void Strobe::set_intensity(std::uint8_t value) {
    value = std::clamp(value, std::uint8_t{0}, m_max_intensity);

    auto pulse = value * strobe_pwm.period / UINT8_MAX;

    // This is non critical code, so we can ignore the return value.
    std::ignore = pwm_set_dt(&strobe_pwm, strobe_pwm.period, pulse);
}


void Strobe::on(std::uint8_t intensity, std::chrono::milliseconds period) {
    k_thread_suspend(strobe);
    if (intensity == 0) {
        off();
        return;
    }

    m_intensity    = std::clamp(intensity, std::uint8_t{0}, m_max_intensity);
    m_blink_period = period;
    m_is_running   = true;
    k_thread_resume(strobe);
}

void Strobe::off() {
    k_thread_suspend(strobe);
    set_intensity(0);
    m_is_running = false;
}

int strobe_thread(void) {
    bool  is_on{false};
    auto& inst = strobe_instance();
    while (1) {
        is_on = !is_on;
        inst.set_intensity(is_on ? inst.m_intensity : 0);
        k_msleep(inst.m_blink_period.count());
    }
}


Strobe& strobe_instance() {
    static Strobe strobe_light;
    return strobe_light;
}
