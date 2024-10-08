#include "fan.hpp"
#include "status_led.hpp"

#include <zephyr/drivers/pwm.h>

#include <algorithm>
#include <chrono>
#include <tuple>

static pwm_dt_spec const led_r = PWM_DT_SPEC_GET(DT_NODELABEL(led_r));
static pwm_dt_spec const led_g = PWM_DT_SPEC_GET(DT_NODELABEL(led_g));
static pwm_dt_spec const led_b = PWM_DT_SPEC_GET(DT_NODELABEL(led_b));


static pwm_dt_spec const fan = PWM_DT_SPEC_GET(DT_NODELABEL(fan));


using namespace std::chrono_literals;
using namespace std::chrono;

constexpr static bool invert_pwm = true;

constexpr static nanoseconds pwm_period = 100us;


constexpr static nanoseconds pulse_width(unsigned value, unsigned max) noexcept {
    value = std::clamp(value, 0u, max);
    value = max - value;
    return value * pwm_period / max;
}


auto Light::set_color(Color color) noexcept -> std::expected<void, error_code> {
    if (!is_output_allowed()) {
        color = Colors::Black;
    }

    auto red_pulse   = pulse_width(color.red, UINT8_MAX).count();
    auto green_pulse = pulse_width(color.green, UINT8_MAX).count();
    auto blue_pulse  = pulse_width(color.blue, UINT8_MAX).count();
    auto period      = pwm_period.count();

    int result = pwm_set_dt(&led_r, period, red_pulse);
    if (0 != result) {
        return std::unexpected(error_code{result});
    }

    result = pwm_set_dt(&led_g, period, green_pulse);
    if (0 != result) {
        return std::unexpected(error_code{result});
    }

    result = pwm_set_dt(&led_b, period, blue_pulse);
    if (0 != result) {
        return std::unexpected(error_code{result});
    }

    return {};
}

void Light::allow_output(bool allowed) noexcept {
    output_allowed = allowed;
    if (!allowed) {
        std::ignore = off();
    }
}


Light& status_light_instance() noexcept {
    static Light status_light;
    return status_light;
}


auto Fan::set_speed(int percentage) noexcept -> std::expected<void, error_code> {
    percentage = std::clamp(percentage, 0, 100);

    auto fan_pulse = pulse_width(percentage, 100).count();
    auto period    = pwm_period.count();

    auto result = pwm_set_dt(&fan, period, fan_pulse);

    if (0 != result) {
        return std::unexpected(error_code{result});
    }

    current_speed = percentage;

    return {};
}


Fan& fan_instance() noexcept {
    static Fan fan;
    return fan;
}
