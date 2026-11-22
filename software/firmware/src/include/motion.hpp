#ifndef GTFOG_MOTION_HPP
#define GTFOG_MOTION_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace gtfog {

// One ADXL345 sample in raw full-resolution counts (3.9 mg per count in every range).
struct accelerometer_sample {
  std::int16_t x, y, z;
};

// Full resolution reads +-4096 counts at most (+-16 g); a corrupt sample is clamped to that.
constexpr int accelerometer_max_counts = 4096;
[[nodiscard]] constexpr auto counts_to_milli_g(std::int16_t counts) noexcept -> std::int16_t {
  int clamped =
      counts > accelerometer_max_counts ? accelerometer_max_counts : (counts < -accelerometer_max_counts ? -accelerometer_max_counts : counts);
  return static_cast<std::int16_t>(clamped * 39 / 10);
}

// Accelerometer stream notification: u16 seq, u8 rate_hz, u8 count, then count x (int16 x, y, z) in mg, little endian.
constexpr std::size_t accelerometer_header_size = 4;
constexpr std::size_t accelerometer_sample_size = 6;
constexpr std::size_t accelerometer_max_samples = 40; // Fills a 247-byte ATT MTU.

// Samples that fit one notification for the given ATT MTU.
[[nodiscard]] constexpr auto accelerometer_samples_per_packet(std::size_t mtu) noexcept -> std::size_t {
  std::size_t payload = mtu > 3 + accelerometer_header_size ? mtu - 3 - accelerometer_header_size : 0;
  std::size_t fit = payload / accelerometer_sample_size;
  return fit < accelerometer_max_samples ? fit : accelerometer_max_samples;
}

// Writes the packet into `out` and returns its length; 0 (nothing written) when `out` is smaller than accelerometer_header_size +
// 6 * samples.size() bytes.
auto pack_accelerometer_samples(std::uint16_t seq, std::uint8_t rate_hz, std::span<accelerometer_sample const> samples,
                                std::span<std::uint8_t> out) noexcept -> std::size_t;

// ADXL345 FIFO_STATUS: bits 0..5 hold the number of samples; 32 means the FIFO is full and the next one overwrites the oldest.
constexpr unsigned fifo_capacity = 32;
[[nodiscard]] constexpr auto fifo_entries(std::uint8_t status) noexcept -> unsigned { return status & 0x3fu; }
[[nodiscard]] constexpr auto fifo_overflowed(std::uint8_t status) noexcept -> bool { return fifo_entries(status) >= fifo_capacity; }

// Notices a sensor that stopped delivering: a poll that fails or finds the FIFO empty is unhealthy; after `stall_time` of
// them the caller power-cycles the sensor, and the third such recovery in a row gives up (`lost`, once; `still_lost` for
// every later failed recovery, until a healthy poll).
class fifo_watchdog {
public:
  static constexpr std::chrono::milliseconds stall_time{1000};
  static constexpr unsigned max_recoveries = 3;
  enum class verdict { ok, recover, lost, still_lost };

  // `elapsed` since the previous poll.
  [[nodiscard]] auto poll(bool healthy, std::chrono::milliseconds elapsed) noexcept -> verdict {
    if(healthy) {
      bad_time = std::chrono::milliseconds{0};
      recoveries = 0;
      return verdict::ok;
    }
    bad_time += elapsed;
    if(bad_time < stall_time) {
      return verdict::ok;
    }
    bad_time = std::chrono::milliseconds{0};
    if(recoveries <= max_recoveries) { // One past the maximum marks "reported"; no further counting.
      ++recoveries;
    }
    return recoveries < max_recoveries ? verdict::recover : (recoveries == max_recoveries ? verdict::lost : verdict::still_lost);
  }

private:
  std::chrono::milliseconds bad_time{0};
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
  [[nodiscard]] auto update(std::span<accelerometer_sample const> samples) noexcept -> std::optional<activity_level>;

  // Drops the partial window, e.g. after a gap in the samples.
  void reset() noexcept { count = 0, mean = m2 = 0; }

private:
  unsigned count{0};
  float mean{0}, m2{0}; // Welford running mean and sum of squared deviations.
};

} // namespace gtfog

#endif
