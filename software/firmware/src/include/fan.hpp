#ifndef FAN_HPP_
#define FAN_HPP_

#include "error_codes.hpp"

#include <expected>


class Fan {
    public:
    /**
     * Set the fan speed.
     * If the requested value is > 100, the fan will be set to 100%.
     * @param[in] percentage The speed of the fan as a percentage.
     */
    [[nodiscard]] auto set_speed(unsigned percentage) noexcept -> std::expected<void, error_code>;

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