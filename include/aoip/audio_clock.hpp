// SPDX-License-Identifier: MIT
#pragma once
#include "json.hpp"
#include <atomic>
#include <string>
namespace aoip {
// Software frequency discipline. Offset is deliberately never used to jump an audio timeline.
// Control-thread update/status; the audio scheduler reads only the atomic correction.
class AudioRateServo {
public:
  void update(const nlohmann::json& ptp, double monotonic_seconds);
  double correction_ppm() const {
    return applied_.load(std::memory_order_relaxed);
  }
  nlohmann::json status() const;

private:
  std::atomic<double> applied_{0};
  double updated_ = -1, target_ = 0;
  bool acquired_ = false, ready_ = false;
  std::string state_ = "Acquiring", reference_;
};
// Deadlines remain in local monotonic time. Positive master-minus-local frequency means
// more network frames per local second, hence a shorter local packet interval.
double disciplined_interval_ms(int sample_rate, double correction_ppm);
} // namespace aoip
