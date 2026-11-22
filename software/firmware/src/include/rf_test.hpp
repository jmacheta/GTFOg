#ifndef GTFOG_RF_TEST_HPP
#define GTFOG_RF_TEST_HPP

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>

namespace gtfog {

// Bluetooth LE channel index 0..39 (37, 38, 39 advertise) to the carrier frequency in MHz; 0 for an invalid index.
[[nodiscard]] constexpr auto rf_test_frequency_mhz(unsigned channel) noexcept -> unsigned {
  if(channel == 37) {
    return 2402;
  }
  if(channel == 38) {
    return 2426;
  }
  if(channel == 39) {
    return 2480;
  }
  if(channel <= 10) {
    return 2404 + 2 * channel;
  }
  return channel <= 36 ? 2428 + 2 * (channel - 11) : 0;
}

// What the RF test characteristic carries: [channel 0..39, seconds 1..255].
struct rf_test_request {
  std::uint8_t channel;
  std::chrono::seconds duration;
};
constexpr std::size_t rf_test_request_size = 2;

[[nodiscard]] constexpr auto is_rf_test_size(std::size_t len) noexcept -> bool { return len == rf_test_request_size; }

[[nodiscard]] constexpr auto decode_rf_test(std::span<std::uint8_t const> bytes) noexcept -> std::optional<rf_test_request> {
  if(!is_rf_test_size(bytes.size()) || rf_test_frequency_mhz(bytes[0]) == 0 || bytes[1] == 0) {
    return std::nullopt;
  }
  return rf_test_request{bytes[0], std::chrono::seconds{bytes[1]}};
}

} // namespace gtfog

#endif
