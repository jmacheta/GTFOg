#ifndef GTFOG_TESTS_FAKE_OUTPUTS_HPP
#define GTFOG_TESTS_FAKE_OUTPUTS_HPP

#include "controller.hpp"

#include <vector>

// Records everything the controller asks of the hardware.
struct fake_outputs final : gtfog::outputs {
  unsigned fan{999};
  bool strobe{true};
  std::vector<gtfog::signal> signals;
  bool powered_off{false};
  std::vector<gtfog::device_settings> saved;
  unsigned fan_calls{0};

  [[nodiscard]] auto saved_count() const -> int { return static_cast<int>(saved.size()); }

  void set_fan(unsigned percent) override {
    fan = percent;
    ++fan_calls;
  }
  void set_strobe(bool on) override { strobe = on; }
  void show(gtfog::signal s) override { signals.push_back(s); }
  void power_off() override { powered_off = true; }
  void settings_changed(gtfog::device_settings const &s) override { saved.push_back(s); }
};

#endif
