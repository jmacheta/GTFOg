#include "motion.hpp"

#include <zephyr/ztest.h>

#include <array>
#include <optional>

using namespace gtfog;

ZTEST_SUITE(motion, nullptr, nullptr, nullptr, nullptr, nullptr);

ZTEST(motion, test_counts_to_milli_g) {
  zassert_equal(counts_to_milli_g(0), 0);
  zassert_equal(counts_to_milli_g(256), 998); // About 1 g.
  zassert_equal(counts_to_milli_g(-256), -998);
  zassert_equal(counts_to_milli_g(4095), 15970);  // +-16 g full scale fits int16.
  zassert_equal(counts_to_milli_g(32767), 15974); // A corrupt sample is clamped, not wrapped.
  zassert_equal(counts_to_milli_g(-32768), -15974);
}

ZTEST(motion, test_samples_per_packet) {
  zassert_equal(accelerometer_samples_per_packet(23), 2); // Default ATT MTU: 20-byte payload.
  zassert_equal(accelerometer_samples_per_packet(247), 40);
  zassert_equal(accelerometer_samples_per_packet(517), 40);
  zassert_equal(accelerometer_samples_per_packet(5), 0);
}

ZTEST(motion, test_pack_layout) {
  std::array<accelerometer_sample, 2> samples{{{256, -256, 0}, {1, 2, -3}}};
  std::array<std::uint8_t, accelerometer_header_size + 2 * accelerometer_sample_size> out{};
  auto len = pack_accelerometer_samples(0x1234, 100, samples, out);
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

namespace {

// One second of samples whose magnitude alternates between 1 g and 1 g + `swing` counts: the deviation is half the swing.
auto classify_second(int swing) -> std::optional<activity_level> {
  activity_classifier classifier;
  std::array<accelerometer_sample, 50> batch{};
  std::optional<activity_level> level;
  for(int half = 0; half != 2; ++half) {
    for(std::size_t i = 0; i != batch.size(); ++i) {
      batch[i] = {0, 0, static_cast<std::int16_t>(256 + (i % 2 == 0 ? 0 : swing))};
    }
    zassert_false(level.has_value(), "a level only after 100 samples");
    level = classifier.update(batch);
  }
  return level;
}

} // namespace

ZTEST(motion, test_activity_levels) {
  zassert_equal(classify_second(0), activity_level::still);
  zassert_equal(classify_second(8), activity_level::still);     // About 31 mg deviation.
  zassert_equal(classify_second(60), activity_level::walking);  // About 117 mg.
  zassert_equal(classify_second(150), activity_level::walking); // About 293 mg.
  zassert_equal(classify_second(200), activity_level::running); // About 390 mg.
}

ZTEST(motion, test_activity_ignores_gravity_and_reports_every_second) {
  activity_classifier classifier;
  std::array<accelerometer_sample, 30> tilted{};
  tilted.fill({256, 0, 0}); // Any constant orientation is still.
  int reports = 0;
  for(int i = 0; i != 20; ++i) {
    if(auto level = classifier.update(tilted)) {
      zassert_equal(*level, activity_level::still);
      ++reports;
    }
  }
  zassert_equal(reports, 6); // 600 samples.
}

ZTEST(motion, test_pack_refuses_a_buffer_that_is_too_small) {
  std::array<accelerometer_sample, 2> samples{};
  std::array<std::uint8_t, accelerometer_header_size + 2 * accelerometer_sample_size - 1> small{};
  small.fill(0xaa);
  zassert_equal(pack_accelerometer_samples(1, 100, samples, small), 0);
  zassert_equal(small[0], 0xaa, "nothing written");
}

ZTEST(motion, test_fifo_status_decoding) {
  zassert_equal(fifo_entries(0x9f), 0x1f);
  zassert_false(fifo_overflowed(0x1f));
  zassert_true(fifo_overflowed(0x20));
}

ZTEST(motion, test_fifo_watchdog_recovers_after_a_second_and_gives_up_on_the_third) {
  fifo_watchdog dog;
  using verdict = fifo_watchdog::verdict;
  for(int i = 0; i != 4; ++i) {
    zassert_equal(dog.poll(false, 200), verdict::ok);
  }
  zassert_equal(dog.poll(false, 200), verdict::recover);
  zassert_equal(dog.poll(true, 200), verdict::ok); // A healthy poll clears the history.
  for(int round = 0; round != 2; ++round) {
    zassert_equal(dog.poll(false, 1000), verdict::recover);
  }
  zassert_equal(dog.poll(false, 1000), verdict::lost);
  zassert_equal(dog.poll(false, 1000), verdict::recover, "lost is reported once");
}

ZTEST(motion, test_activity_thresholds_are_80_and_300_mg) {
  // Swings of 40 and 42 counts alternate magnitudes 156 and 164 mg apart: deviations 78 and 82 mg; 153 and 154 counts: 298.5 and 300.5 mg.
  zassert_equal(classify_second(40), activity_level::still);
  zassert_equal(classify_second(42), activity_level::walking);
  zassert_equal(classify_second(153), activity_level::walking);
  zassert_equal(classify_second(154), activity_level::running);
}

ZTEST(motion, test_activity_window_may_span_chunks_of_any_size) {
  activity_classifier classifier;
  std::array<accelerometer_sample, 30> chunk{};
  int reports = 0;
  std::optional<activity_level> last;
  for(int i = 0; i != 7; ++i) { // 210 samples: windows complete inside chunks 4 and 7.
    for(std::size_t j = 0; j != chunk.size(); ++j) {
      chunk[j] = {0, 0, static_cast<std::int16_t>(256 + ((i * chunk.size() + j) % 2 == 0 ? 0 : 200))};
    }
    if(auto level = classifier.update(chunk)) {
      last = level;
      ++reports;
    }
  }
  zassert_equal(reports, 2);
  zassert_equal(*last, activity_level::running);
}
