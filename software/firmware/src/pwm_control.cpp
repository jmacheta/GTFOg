#include "fan.hpp"
#include "status_light.hpp"
#include "strobe_light.hpp"

#include <fmt/compile.h>
#include <fmt/core.h>
#include <zephyr/drivers/pwm.h>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <tuple>


using namespace fmt::literals;


using namespace std::chrono_literals;
using namespace std::chrono;


constexpr static nanoseconds pwm_period = 10ms;


constexpr static nanoseconds pulse_width(unsigned value, unsigned max) noexcept {
    value = std::clamp(value, 0u, max);
    // value = max - value;
    return value * pwm_period / max;
}


void StatusLight::set_color(Color color) {
    static pwm_dt_spec const led_r = PWM_DT_SPEC_GET(DT_NODELABEL(led_r));
    static pwm_dt_spec const led_g = PWM_DT_SPEC_GET(DT_NODELABEL(led_g));
    static pwm_dt_spec const led_b = PWM_DT_SPEC_GET(DT_NODELABEL(led_b));


    if (!is_output_allowed()) {
        color = Colors::Black;
    }

    auto period = pwm_period.count();

    auto red_pulse   = pulse_width(color.red, UINT8_MAX).count();
    auto green_pulse = pulse_width(color.green, UINT8_MAX).count();
    auto blue_pulse  = pulse_width(color.blue, UINT8_MAX).count();

    // This is absolutely non critical code, so we can ignore the return value.
    std::ignore = pwm_set_dt(&led_r, led_r.period, red_pulse);
    std::ignore = pwm_set_dt(&led_g, period, green_pulse);
    std::ignore = pwm_set_dt(&led_b, period, blue_pulse);
}

void StatusLight::allow_output(bool allowed) {
    output_allowed = allowed;
    if (!allowed) {
        off();
    }
}


StatusLight& status_light_instance() noexcept {
    static StatusLight status_light;
    return status_light;
}


void StrobeLight::set_intensity(std::uint8_t value) {
    static pwm_dt_spec const strobe = PWM_DT_SPEC_GET(DT_NODELABEL(strobe));

    if (!is_output_allowed()) {
        value = 0;
    }


    intensity = std::clamp(value, uint8_t{0}, uint8_t{255});
    printf("period: %d\n", strobe.period);
    auto pulse = value * strobe.period / 255;

    // This is absolutely non critical code, so we can ignore the return value.
    std::ignore = pwm_set_dt(&strobe, strobe.period, pulse);
}

void StrobeLight::allow_output(bool allowed) {
    output_allowed = allowed;
    if (!allowed) {
        off();
    }
}

StrobeLight& strobe_light_instance() noexcept {
    static StrobeLight strobe_light;
    return strobe_light;
}


void Fan::set_speed(int percentage) {
    static pwm_dt_spec const fan = PWM_DT_SPEC_GET(DT_NODELABEL(fan));

    percentage = std::clamp(percentage, 0, 100);

    auto fan_pulse = percentage * fan.period / 100;

    auto result = pwm_set_dt(&fan, fan.period, fan_pulse);

    if (0 != result) {
        throw std::runtime_error{fmt::format("Fan PWM error: {}"_cf, result)};
    }

    current_speed = percentage;
}


Fan& fan_instance() noexcept {
    static Fan fan;
    return fan;
}
