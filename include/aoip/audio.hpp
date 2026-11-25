#pragma once
#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace aoip {
inline constexpr int sample_rate = 48000, channel_count = 2, frames_per_packet = 240;
inline constexpr int packet_ms = 5, payload_bytes = 960, header_bytes = 12;
using Pcm = std::array<std::uint8_t, payload_bytes>;
using Bytes = std::vector<std::uint8_t>;
struct Packet {
  std::uint16_t sequence{};
  std::uint32_t timestamp{}, ssrc{};
  Pcm pcm{};
};
Bytes encode_packet(const Packet& packet);
Packet decode_packet(std::span<const std::uint8_t> bytes);
int sequence_distance(std::uint16_t a, std::uint16_t b);
bool supported_sample_rate(int rate);
double packet_duration_ms(int rate);
Pcm tone_packet(std::uint64_t frame_offset, double frequency, int rate = sample_rate);
struct Levels {
  double rms{}, peak{};
};
Levels levels(const Pcm& pcm);
Bytes encode_wav(std::span<const std::uint8_t> pcm, int rate = sample_rate);
Bytes decode_wav(std::span<const std::uint8_t> wav, int rate = sample_rate);
Pcm wav_packet(const Bytes& pcm, std::uint64_t offset_frames);
Bytes load_wav(const std::string& path, int rate = sample_rate);

struct JitterStats {
  std::uint64_t received{}, lost{}, late{}, duplicates{}, reordered{}, resets{};
  double jitter_ms{};
};
class JitterBuffer {
public:
  explicit JitterBuffer(int latency_ms = 60, int rate = sample_rate);
  void reset();
  void push(const Packet& packet, double now_ms);
  std::optional<Pcm> take(double now_ms);
  JitterStats stats;
  bool playing() const {
    return playing_;
  }
  std::size_t queued() const {
    return queue_.size();
  }

private:
  int latency_ms_;
  int sample_rate_;
  std::map<std::uint16_t, Pcm> queue_;
  std::optional<std::uint16_t> expected_, last_sequence_;
  std::optional<double> last_arrival_;
  std::uint32_t last_timestamp_{};
  double start_at_{};
  bool playing_{};
};
class AudioRing {
public:
  explicit AudioRing(int rate = sample_rate) : rate_(rate) {}
  void push(const Pcm& pcm);
  void reset() {
    blocks_.clear();
  }
  Bytes snapshot() const;
  double seconds() const {
    return blocks_.size() * frames_per_packet / double(rate_);
  }

private:
  std::deque<Pcm> blocks_;
  int rate_;
};
} // namespace aoip
