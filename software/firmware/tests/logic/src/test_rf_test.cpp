#include "rf_test.hpp"

#include <zephyr/ztest.h>

#include <array>

using namespace gtfog;

ZTEST_SUITE(rf_test, nullptr, nullptr, nullptr, nullptr, nullptr);

ZTEST(rf_test, test_frequency_mapping) {
  zassert_equal(rf_test_frequency_mhz(37), 2402);
  zassert_equal(rf_test_frequency_mhz(38), 2426);
  zassert_equal(rf_test_frequency_mhz(39), 2480);
  zassert_equal(rf_test_frequency_mhz(0), 2404);
  zassert_equal(rf_test_frequency_mhz(10), 2424);
  zassert_equal(rf_test_frequency_mhz(11), 2428);
  zassert_equal(rf_test_frequency_mhz(19), 2444);
  zassert_equal(rf_test_frequency_mhz(36), 2478);
  zassert_equal(rf_test_frequency_mhz(40), 0);
  zassert_equal(rf_test_frequency_mhz(255), 0);
}

ZTEST(rf_test, test_frequencies_are_distinct_and_even_spaced) {
  unsigned seen[40]{};
  for(unsigned channel = 0; channel < 40; ++channel) {
    auto mhz = rf_test_frequency_mhz(channel);
    zassert_true(mhz >= 2402 && mhz <= 2480 && mhz % 2 == 0);
    ++seen[(mhz - 2402) / 2];
  }
  for(auto count : seen) {
    zassert_equal(count, 1); // 40 channels on the 40 slots of a 2 MHz grid.
  }
}

ZTEST(rf_test, test_decode) {
  std::array<std::uint8_t, 2> good{19, 30};
  auto request = decode_rf_test(good);
  zassert_true(request.has_value());
  zassert_equal(request->channel, 19);
  zassert_equal(request->duration.count(), 30);
  std::array<std::uint8_t, 2> longest{39, 255};
  zassert_true(decode_rf_test(longest).has_value());
}

ZTEST(rf_test, test_decode_rejects) {
  std::array<std::uint8_t, 2> bad_channel{40, 30};
  std::array<std::uint8_t, 2> zero_seconds{5, 0};
  std::array<std::uint8_t, 1> short_payload{5};
  std::array<std::uint8_t, 3> long_payload{5, 30, 0};
  zassert_false(decode_rf_test(bad_channel).has_value());
  zassert_false(decode_rf_test(zero_seconds).has_value());
  zassert_false(decode_rf_test(short_payload).has_value());
  zassert_false(decode_rf_test(long_payload).has_value());
  zassert_true(is_rf_test_size(2) && !is_rf_test_size(1));
}
