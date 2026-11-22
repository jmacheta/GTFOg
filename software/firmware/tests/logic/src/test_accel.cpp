#include "accel.hpp"

#include <zephyr/ztest.h>

#include <array>

using namespace gtfog;

ZTEST_SUITE(accel, nullptr, nullptr, nullptr, nullptr, nullptr);

ZTEST(accel, test_counts_to_milli_g) {
  zassert_equal(counts_to_milli_g(0), 0);
  zassert_equal(counts_to_milli_g(256), 998); // About 1 g.
  zassert_equal(counts_to_milli_g(-256), -998);
  zassert_equal(counts_to_milli_g(4095), 15970); // +-16 g full scale fits int16.
}

ZTEST(accel, test_samples_per_packet) {
  zassert_equal(accel_samples_per_packet(23), 2); // Default ATT MTU: 20-byte payload.
  zassert_equal(accel_samples_per_packet(247), 40);
  zassert_equal(accel_samples_per_packet(517), 40);
  zassert_equal(accel_samples_per_packet(5), 0);
}

ZTEST(accel, test_pack_layout) {
  std::array<accel_sample, 2> samples{{{256, -256, 0}, {1, 2, -3}}};
  std::array<std::uint8_t, accel_header_size + 2 * accel_sample_size> out{};
  auto len = pack_accel(0x1234, 100, samples, out);
  zassert_equal(len, out.size());
  zassert_equal(out[0], 0x34);
  zassert_equal(out[1], 0x12);
  zassert_equal(out[2], 100);
  zassert_equal(out[3], 2);
  zassert_equal(static_cast<std::int16_t>(out[4] | out[5] << 8), 998);
  zassert_equal(static_cast<std::int16_t>(out[6] | out[7] << 8), -998);
  zassert_equal(static_cast<std::int16_t>(out[8] | out[9] << 8), 0);
  zassert_equal(static_cast<std::int16_t>(out[14] | out[15] << 8), -11); // -3 counts = -11.7 mg, truncated.
}
