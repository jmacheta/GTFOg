#ifndef STROBE_HPP_
#define STROBE_HPP_

#include <chrono>
#include <cstdint>

class Strobe {
  public:
    Strobe(std::uint8_t max_intensity = 255U) : m_max_intensity(max_intensity) {
    }

    bool is_on() const noexcept {
        return m_is_running;
    }

    void on(std::uint8_t intensity, std::chrono::milliseconds period);
    void off();

  protected:
    /**
     * Set the color of the light.
     * @param value The color to set.
     * @return An error code if the operation failed.
     */
    void set_intensity(std::uint8_t value);

    ~Strobe() = default;
    friend Strobe&            strobe_instance();
    friend int                strobe_thread();
    std::chrono::milliseconds m_blink_period{500};

    std::uint8_t m_max_intensity{255};
    std::uint8_t m_intensity{0};
    bool         m_is_running{false};
};

/**
 * Get the status light instance.
 * @return The status light instance.
 */
[[nodiscard]] Strobe& strobe_instance();


#endif
