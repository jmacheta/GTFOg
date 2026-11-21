#include "accel.hpp"

namespace gtfog {

namespace {

auto put16(std::uint8_t *at, std::int32_t value) noexcept -> std::uint8_t * {
  auto v = static_cast<std::uint16_t>(value);
  at[0] = static_cast<std::uint8_t>(v & 0xff);
  at[1] = static_cast<std::uint8_t>(v >> 8);
  return at + 2;
}

} // namespace

auto pack_accel(std::uint16_t seq, std::uint8_t rate_hz, std::span<accel_sample const> samples, std::span<std::uint8_t> out) noexcept -> std::size_t {
  auto *at = put16(out.data(), seq);
  *at++ = rate_hz;
  *at++ = static_cast<std::uint8_t>(samples.size());
  for(auto const &s : samples) {
    at = put16(at, counts_to_milli_g(s.x));
    at = put16(at, counts_to_milli_g(s.y));
    at = put16(at, counts_to_milli_g(s.z));
  }
  return static_cast<std::size_t>(at - out.data());
}

} // namespace gtfog
