#ifndef FAN_HPP_
#define FAN_HPP_
#include <cstdint>

class Fan {
    std::uint8_t min_speed{0};
    std::uint8_t speed{0};
    std::uint8_t max_speed{255};

  public:
    [[nodiscard]] constexpr auto get_speed() const noexcept {
        return speed;
    }


    void set_limits(std::uint8_t min, std::uint8_t max = 255u);

    /**
     * @param[in] percentage The speed of the fan as a percentage.
     */
    void set_speed(std::uint8_t percentage);

    /**
     * Turn the fan off.
     */
    auto off() {
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