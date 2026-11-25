#include "aoip/adaptive_buffer.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace aoip {
AdaptiveBuffer::AdaptiveBuffer(int channels, int rate, int ms) : channels_(channels), rate_(rate) {
  if (channels < 1 || channels > 32 || rate < 8000 || rate > 192000 || ms < 5 || ms > 500)
    throw std::runtime_error("Invalid adaptive audio buffer dimensions");
  target_ = std::max<std::size_t>(2, rate * ms / 1000);
  capacity_ = std::max<std::size_t>(target_ * 4, rate);
  data_.resize(capacity_ * channels);
}
std::size_t AdaptiveBuffer::available() const {
  const auto r = read_.load(std::memory_order_acquire), w = write_.load(std::memory_order_acquire);
  return static_cast<std::size_t>(std::min<std::uint64_t>(w - r, capacity_));
}
std::size_t AdaptiveBuffer::write(std::span<const float> input) {
  if (input.size() % channels_)
    return 0;
  const auto w = write_.load(std::memory_order_relaxed), r = read_.load(std::memory_order_acquire);
  const auto requested = input.size() / channels_;
  const auto count =
      std::min<std::size_t>(requested, capacity_ - std::min<std::size_t>(w - r, capacity_));
  for (std::size_t f = 0; f < count; ++f)
    for (int c = 0; c < channels_; ++c) {
      const float value = input[f * channels_ + c];
      data_[((w + f) % capacity_) * channels_ + c] = std::isfinite(value) ? value : 0.f;
    }
  write_.store(w + count, std::memory_order_release);
  dropped_.fetch_add(requested - count, std::memory_order_relaxed);
  return count;
}
std::size_t AdaptiveBuffer::read(std::span<float> output) {
  if (output.size() % channels_)
    return 0;
  auto r = read_.load(std::memory_order_relaxed);
  const auto w = write_.load(std::memory_order_acquire);
  auto queued = std::min<std::size_t>(w - r, capacity_);
  const auto frames = output.size() / channels_;
  // After a long producer-only interval, resume near the live edge instead of playing stale audio.
  if (queued > std::max(target_ * 3, target_ + frames * 4)) {
    const auto skipped = queued - std::min(queued, target_ + frames + 2);
    r += skipped;
    queued -= skipped;
    phase_ = 0;
    dropped_.fetch_add(skipped, std::memory_order_relaxed);
  }
  std::fill(output.begin(), output.end(), 0.f);
  if (!primed_) {
    if (queued < std::max(target_, frames + 2))
      return 0;
    primed_ = true;
  }
  // A bounded, slowly varying rate correction avoids periodically dropping whole samples/blocks.
  const double desired =
      std::clamp((double(queued) - double(target_ + frames)) / rate_ * 100000., -2000., 2000.);
  const double old = ppm_.load(std::memory_order_relaxed);
  const double ppm = old + std::clamp(desired - old, -4., 4.);
  ppm_.store(ppm, std::memory_order_relaxed);
  const double step = 1. + ppm * 1e-6;
  std::size_t produced = 0;
  for (; produced < frames; ++produced) {
    const auto at = static_cast<std::size_t>(phase_);
    if (at + 1 >= queued)
      break;
    const auto fraction = static_cast<float>(phase_ - at);
    for (int c = 0; c < channels_; ++c) {
      const auto a = data_[((r + at) % capacity_) * channels_ + c],
                 b = data_[((r + at + 1) % capacity_) * channels_ + c];
      output[produced * channels_ + c] = a + (b - a) * fraction;
    }
    phase_ += step;
  }
  const auto consumed = std::min<std::size_t>(static_cast<std::size_t>(phase_), queued);
  phase_ -= consumed;
  read_.store(r + consumed, std::memory_order_release);
  if (produced < frames) {
    underruns_.fetch_add(frames - produced, std::memory_order_relaxed);
    primed_ = false;
    phase_ = 0;
  }
  return produced;
}
} // namespace aoip
