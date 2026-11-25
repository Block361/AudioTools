#pragma once
#include <atomic>
#include <cstdint>
#include <span>
#include <vector>
namespace aoip {
// SPSC queue plus continuous linear interpolation. One producer, one consumer.
// Hardware-rate conversion happens in the native endpoint; this servo handles residual clock drift.
class AdaptiveBuffer {
public:
  AdaptiveBuffer(int channels, int sample_rate, int target_ms);
  std::size_t write(std::span<const float>);
  std::size_t read(std::span<float>);
  std::size_t available() const;
  double drift_ppm() const {
    return ppm_.load(std::memory_order_relaxed);
  }
  std::uint64_t dropped() const {
    return dropped_.load();
  }
  std::uint64_t underruns() const {
    return underruns_.load();
  }
  int channels() const {
    return channels_;
  }

private:
  int channels_, rate_;
  std::size_t capacity_, target_;
  std::vector<float> data_;
  alignas(64) std::atomic<std::uint64_t> read_{0}, write_{0};
  std::atomic<std::uint64_t> dropped_{0}, underruns_{0};
  std::atomic<double> ppm_{0};
  double phase_ = 0;
  bool primed_ = false;
};
} // namespace aoip
