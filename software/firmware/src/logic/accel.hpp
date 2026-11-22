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

// Full resolution reads +-4096 counts at most (+-16 g); a corrupt sample is clamped to that.
constexpr int accel_max_counts = 4096;
[[nodiscard]] constexpr auto counts_to_milli_g(std::int16_t counts) noexcept -> std::int16_t {
  int clamped = counts > accel_max_counts ? accel_max_counts : (counts < -accel_max_counts ? -accel_max_counts : counts);
  return static_cast<std::int16_t>(clamped * 39 / 10);
}

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

// Writes the packet into `out` and returns its length; 0 (nothing written) when `out` is smaller than accel_header_size +
// 6 * samples.size() bytes.
auto pack_accel(std::uint16_t seq, std::uint8_t rate_hz, std::span<accel_sample const> samples, std::span<std::uint8_t> out) noexcept -> std::size_t;

// ADXL345 FIFO_STATUS: bits 0..5 hold the number of samples; 32 means the FIFO is full and the next one overwrites the oldest.
constexpr unsigned fifo_capacity = 32;
[[nodiscard]] constexpr auto fifo_entries(std::uint8_t status) noexcept -> unsigned { return status & 0x3fu; }
[[nodiscard]] constexpr auto fifo_overflowed(std::uint8_t status) noexcept -> bool { return fifo_entries(status) >= fifo_capacity; }

// Notices a sensor that stopped delivering: a poll that fails or finds the FIFO empty is unhealthy; after `stall_ms` of
// them the caller power-cycles the sensor, and the third such recovery in a row gives up (reported once).
class fifo_watchdog {
public:
  static constexpr unsigned stall_ms = 1000;
  static constexpr unsigned max_recoveries = 3;
  enum class verdict { ok, recover, lost };

  // `elapsed_ms` since the previous poll.
  [[nodiscard]] auto poll(bool healthy, unsigned elapsed_ms) noexcept -> verdict {
    if(healthy) {
      bad_ms = recoveries = 0;
      return verdict::ok;
    }
    bad_ms += elapsed_ms;
    if(bad_ms < stall_ms) {
      return verdict::ok;
    }
    bad_ms = 0;
    return ++recoveries == max_recoveries ? verdict::lost : verdict::recover;
  }

private:
  unsigned bad_ms{0};
  unsigned recoveries{0};
};

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
