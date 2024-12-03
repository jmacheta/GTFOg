#ifndef STROBE_LIGHT_HPP_
#define STROBE_LIGHT_HPP_

#include <cstdint>

class StrobeLight {
    bool output_allowed{true};

  public:
    /**
     * Allow or disallow light output.
     * @param allowed True to allow output, false to disallow.
     */
    void allow_output(bool allowed);

    /**
     * Check if output is allowed.
     * @return True if output is allowed, false otherwise.
     */
    [[nodiscard]] constexpr bool is_output_allowed() const noexcept {
        return output_allowed;
    }

    /**
     * Set the color of the light.
     * @param value The color to set.
     * @return An error code if the operation failed.
     */
    void set_intensity(std::uint8_t value);

    /**
     * Turn the light off.
     * @return An error code if the operation failed.
     */
    auto off() {
        return set_intensity(0U);
    }


  protected:
    ~StrobeLight() = default;
    friend StrobeLight& strobe_light_instance() noexcept;
};

/**
 * Get the status light instance.
 * @return The status light instance.
 */
[[nodiscard]] StrobeLight& strobe_light_instance() noexcept;


#endif
