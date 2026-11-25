// SPDX-License-Identifier: MIT
#include "aoip/audio_clock.hpp"
#include "aoip/audio.hpp"
#include <algorithm>
#include <cmath>
namespace aoip {
double disciplined_interval_ms(int rate, double ppm) {
  if (!std::isfinite(ppm))
    ppm = 0;
  return packet_duration_ms(rate) / (1. + std::clamp(ppm, -1000., 1000.) * 1e-6);
}
void AudioRateServo::update(const nlohmann::json& p, double now) {
  if (!std::isfinite(now))
    return;
  const double elapsed = updated_ < 0 ? 0 : std::clamp(now - updated_, 0., .5);
  updated_ = now;
  const auto reference = p.value("grandmasterIdentity", nlohmann::json{});
  const std::string identity = reference.is_string() ? reference.get<std::string>() : "";
  const bool changed = identity != reference_;
  if (changed) {
    reference_ = identity;
    ready_ = false;
  }
  const bool healthy = p.value("error", std::string{}).empty();
  const bool gm = p.value("source", "") == "ptp-grandmaster";
  const auto frequency = p.value("frequencyPpm", nlohmann::json{});
  const bool valid =
      healthy &&
      (gm ? p.value("transmitting", false) && p.value("otherGrandmasters", 0) == 0
          : !changed && p.value("trackingStable", false) && p.value("measurementFresh", false) &&
                frequency.is_number() && std::isfinite(frequency.get<double>()) &&
                std::abs(frequency.get<double>()) <= 1000.);
  if (valid) {
    target_ = gm ? 0 : frequency.get<double>();
    acquired_ = true;
  }
  // Freeze the last applied rate on stale/noisy measurements and master changes.
  // No silence, buffer reset or step in sample position when a grandmaster disappears.
  const auto old = correction_ppm();
  if (!valid)
    target_ = old;
  const auto applied = old + std::clamp(target_ - old, -20. * elapsed, 20. * elapsed);
  applied_.store(applied, std::memory_order_relaxed);
  ready_ = valid && std::abs(target_ - applied) < 1.;
  state_ = valid       ? (ready_ ? (gm ? "Reference" : "Rate locked") : "Acquiring")
           : acquired_ ? "Holdover"
                       : "Acquiring";
}
nlohmann::json AudioRateServo::status() const {
  return {{"rateState", state_},
          {"rateReferenceReady", ready_},
          {"rateCorrectionPpm", correction_ppm()},
          {"rateTargetPpm", target_},
          {"rateLimitPpm", 1000},
          {"rateSlewPpmPerSecond", 20},
          {"phaseSynchronized", false}};
}
} // namespace aoip
