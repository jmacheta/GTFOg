#ifndef GTFOG_LOGIC_ACCEL_HPP
#define GTFOG_LOGIC_ACCEL_HPP

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace gtfog {

// One ADXL345 sample in raw full-resolution counts (3.9 mg per count in every range).
struct accel_sample {
  std::int16_t x, y, z;
};

[[nodiscard]] constexpr auto counts_to_milli_g(std::int16_t counts) noexcept -> std::int16_t { return static_cast<std::int16_t>(counts * 39 / 10); }

// Accelerometer stream notification: u16 seq, u8 rate_hz, u8 count, then count x (int16 x, y, z) in mg, little endian.
constexpr std::size_t accel_header_size = 4;
constexpr std::size_t accel_sample_size = 6;
constexpr std::size_t accel_max_samples = 40; // Fills a 247-byte ATT MTU.

// Samples that fit one notification for the given ATT MTU.
[[nodiscard]] constexpr auto accel_samples_per_packet(std::size_t mtu) noexcept -> std::size_t {
  std::size_t payload = mtu > 3 + accel_header_size ? mtu - 3 - accel_header_size : 0;
  std::size_t fit = payload / accel_sample_size;
  return fit < accel_max_samples ? fit : accel_max_samples;
}

// Writes the packet into `out` and returns its length; `out` must hold accel_header_size + 6 * samples.size() bytes.
auto pack_accel(std::uint16_t seq, std::uint8_t rate_hz, std::span<accel_sample const> samples, std::span<std::uint8_t> out) noexcept -> std::size_t;

enum class activity_level : std::uint8_t { still = 0, walking = 1, running = 2 };

// Classifies the head's activity once per second of samples (100 Hz): the standard deviation of the acceleration
// magnitude, in mg, below `walk_threshold_milli_g` is still, below `run_threshold_milli_g` walking, else running.
class activity_classifier {
public:
  // ponytail: calibrate against Debug-tab recordings of a head at rest, walking and running.
  static constexpr float walk_threshold_milli_g = 80;
  static constexpr float run_threshold_milli_g = 300;
  static constexpr unsigned window_samples = 100;

  // The level of the last window that these samples completed, if any.
  [[nodiscard]] auto update(std::span<accel_sample const> samples) noexcept -> std::optional<activity_level>;

private:
  unsigned count{0};
  float mean{0}, m2{0}; // Welford running mean and sum of squared deviations.
};

} // namespace gtfog

#endif
