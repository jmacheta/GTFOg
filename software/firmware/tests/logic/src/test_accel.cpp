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

ZTEST(accel, test_motion_detector_ignores_noise_at_rest) {
  motion_detector detector;
  std::array<accel_sample, 32> batch{};
  for(int round = 0; round != 20; ++round) {
    for(std::size_t i = 0; i != batch.size(); ++i) {
      auto noise = static_cast<std::int16_t>((static_cast<int>(i) * 7 + round) % 9 - 4); // +-4 counts, about +-16 mg.
      batch[i] = {noise, static_cast<std::int16_t>(-noise), static_cast<std::int16_t>(256 + noise)};
    }
    zassert_false(detector.update(batch));
  }
}

ZTEST(accel, test_motion_detector_sees_movement) {
  motion_detector detector;
  std::array<accel_sample, 32> still{};
  still.fill({0, 0, 256});
  zassert_false(detector.update(still));
  std::array<accel_sample, 32> shake = still;
  shake[10] = {200, 0, 256}; // About 0.27 g more than at rest.
  zassert_true(detector.update(shake));
  zassert_false(detector.update(still));
  shake[10] = {0, 0, 120}; // Free fall towards zero.
  zassert_true(detector.update(shake));
}

ZTEST(accel, test_motion_detector_follows_slow_drift) {
  motion_detector detector;
  accel_sample sample{0, 0, 256};
  zassert_false(detector.update(std::span{&sample, 1}));
  bool moved = false;
  for(int i = 0; i != 2000; ++i) { // The magnitude creeps up by 0.4 g over 2000 samples.
    sample.z = static_cast<std::int16_t>(256 + i / 20);
    moved = moved || detector.update(std::span{&sample, 1});
  }
  zassert_false(moved);
}
