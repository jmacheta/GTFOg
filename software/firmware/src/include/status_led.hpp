#ifndef STATUS_LED_HPP_
#define STATUS_LED_HPP_

#include "error_codes.hpp"

#include <expected>

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
class Light {
    bool output_allowed{true};

  public:
    /**
     * Allow or disallow light output.
     * @param allowed True to allow output, false to disallow.
     */
    void allow_output(bool allowed) noexcept;

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
    [[nodiscard]] auto set_color(Color color) noexcept -> std::expected<void, error_code>;

    /**
     * Turn the light off.
     * @return An error code if the operation failed.
     */
    auto off() noexcept -> std::expected<void, error_code> {
        return set_color(Colors::Black);
    }


  protected:
    ~Light() = default;
    friend Light& status_light_instance() noexcept;
};

/**
 * Get the status light instance.
 * @return The status light instance.
 */
[[nodiscard]] Light& status_light_instance() noexcept;

#endif