#ifndef STATUS_LIGHT_HPP_
#define STATUS_LIGHT_HPP_

#include "error_codes.hpp"

#include <cstdint>


/// RGB color representation.
struct Color {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

/// Predefined colors.
namespace Colors {
    constexpr Color Red{255, 0, 0};
    constexpr Color Green{0, 255, 0};
    constexpr Color Blue{0, 0, 255};
    constexpr Color Yellow{255, 255, 0};
    constexpr Color Cyan{0, 255, 255};
    constexpr Color Magenta{255, 0, 255};
    constexpr Color White{255, 255, 255};
    constexpr Color Black{0, 0, 0};
} // namespace Colors

/// Status light control.
class StatusLight {
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
     * @param color The color to set.
     * @return An error code if the operation failed.
     */
    void set_color(Color color);

    /**
     * Turn the light off.
     * @return An error code if the operation failed.
     */
    auto off() {
        return set_color(Colors::Black);
    }


  protected:
    ~StatusLight() = default;
    friend StatusLight& status_light_instance() noexcept;
};

/**
 * Get the status light instance.
 * @return The status light instance.
 */
[[nodiscard]] StatusLight& status_light_instance() noexcept;

#endif