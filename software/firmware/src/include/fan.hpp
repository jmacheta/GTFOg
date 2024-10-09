#ifndef FAN_HPP_
#define FAN_HPP_

#include "error_codes.hpp"

#include <expected>


class Fan {
    int current_speed = 0;

  public:
    [[nodiscard]] constexpr auto get_speed() const noexcept {
        return current_speed;
    }
    /**
     * Set the fan speed.
     * If the requested value is > 100, the fan will be set to 100%.
     * If the requested value is < 0, the fan will be turned off.
     * @param[in] percentage The speed of the fan as a percentage.
     */
    [[nodiscard]] auto set_speed(int percentage) noexcept -> std::expected<void, error_code>;

    /**
     * Turn the fan off.
     */
    auto off() noexcept -> std::expected<void, error_code> {
        return set_speed(0);
    }

  protected:
    ~Fan() = default;
    friend Fan& fan_instance() noexcept;
};

/**
 * Get the fan instance.
 * @return The fan instance.
 */
[[nodiscard]] Fan& fan_instance() noexcept;


#endif