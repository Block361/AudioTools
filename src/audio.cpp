#include "aoip/audio.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace aoip {
namespace {
std::uint16_t be16(std::span<const std::uint8_t> b, std::size_t p) {
  return static_cast<std::uint16_t>((b[p] << 8) | b[p + 1]);
}
std::uint32_t be32(std::span<const std::uint8_t> b, std::size_t p) {
  return (std::uint32_t(b[p]) << 24) | (std::uint32_t(b[p + 1]) << 16) |
         (std::uint32_t(b[p + 2]) << 8) | b[p + 3];
}
void put16(Bytes& b, std::size_t p, std::uint16_t n) {
  b[p] = std::uint8_t(n >> 8);
  b[p + 1] = std::uint8_t(n);
}
void put32(Bytes& b, std::size_t p, std::uint32_t n) {
  for (int i = 0; i < 4; ++i)
    b[p + i] = std::uint8_t(n >> ((3 - i) * 8));
}
std::uint32_t le32(std::span<const std::uint8_t> b, std::size_t p) {
  return std::uint32_t(b[p]) | (std::uint32_t(b[p + 1]) << 8) | (std::uint32_t(b[p + 2]) << 16) |
         (std::uint32_t(b[p + 3]) << 24);
}
std::uint16_t le16(std::span<const std::uint8_t> b, std::size_t p) {
  return std::uint16_t(b[p] | (b[p + 1] << 8));
}
void little(Bytes& b, std::size_t p, std::uint32_t n, int size) {
  for (int i = 0; i < size; ++i)
    b[p + i] = std::uint8_t(n >> (i * 8));
}
bool tag(std::span<const std::uint8_t> b, std::size_t p, const std::string& text) {
  return std::equal(text.begin(), text.end(), b.begin() + p);
}
} // namespace
Bytes encode_packet(const Packet& p) {
  Bytes bytes(header_bytes + payload_bytes);
  bytes[0] = 0x80;
  bytes[1] = 96;
  put16(bytes, 2, p.sequence);
  put32(bytes, 4, p.timestamp);
  put32(bytes, 8, p.ssrc);
  std::copy(p.pcm.begin(), p.pcm.end(), bytes.begin() + header_bytes);
  return bytes;
}
Packet decode_packet(std::span<const std::uint8_t> b) {
  if (b.size() != header_bytes + payload_bytes || b[0] != 0x80 || (b[1] & 0x7f) != 96)
    throw std::runtime_error("Unsupported RTP packet");
  Packet p;
  p.sequence = be16(b, 2);
  p.timestamp = be32(b, 4);
  p.ssrc = be32(b, 8);
  std::copy(b.begin() + header_bytes, b.end(), p.pcm.begin());
  return p;
}
int sequence_distance(std::uint16_t a, std::uint16_t b) {
  return ((int(a) - int(b) + 32768) & 0xffff) - 32768;
}
bool supported_sample_rate(int rate) {
  return rate == 44100 || rate == 48000 || rate == 96000;
}
double packet_duration_ms(int rate) {
  if (!supported_sample_rate(rate))
    throw std::runtime_error("Supported sample rates: 44100, 48000, 96000 Hz");
  return frames_per_packet * 1000.0 / rate;
}
Pcm tone_packet(std::uint64_t offset, double frequency, int rate) {
  Pcm pcm;
  for (int i = 0; i < frames_per_packet; ++i) {
    const auto sample = std::uint16_t(std::int16_t(std::lround(
        std::sin(2 * std::numbers::pi * frequency * double(offset + i) / rate) * 6500)));
    for (int c = 0; c < 2; ++c) {
      pcm[i * 4 + c * 2] = std::uint8_t(sample >> 8);
      pcm[i * 4 + c * 2 + 1] = std::uint8_t(sample);
    }
  }
  return pcm;
}
Levels levels(const Pcm& pcm) {
  double sum = 0, peak = 0;
  for (std::size_t i = 0; i < pcm.size(); i += 2) {
    const int value = int(be16(pcm, i));
    const double sample = (value >= 32768 ? value - 65536 : value) / 32768.0;
    sum += sample * sample;
    peak = std::max(peak, std::abs(sample));
  }
  return {std::sqrt(sum / (pcm.size() / 2)), peak};
}
Bytes encode_wav(std::span<const std::uint8_t> pcm, int rate) {
  if (!supported_sample_rate(rate))
    throw std::runtime_error("Unsupported WAV rate");
  if (pcm.size() % 4 || pcm.size() > std::numeric_limits<std::uint32_t>::max() - 36)
    throw std::runtime_error("Invalid PCM data");
  Bytes wav(44 + pcm.size());
  auto text = [&](std::size_t p, const std::string& s) {
    std::copy(s.begin(), s.end(), wav.begin() + p);
  };
  text(0, "RIFF");
  little(wav, 4, static_cast<std::uint32_t>(36 + pcm.size()), 4);
  text(8, "WAVEfmt ");
  little(wav, 16, 16, 4);
  little(wav, 20, 1, 2);
  little(wav, 22, 2, 2);
  little(wav, 24, rate, 4);
  little(wav, 28, rate * 4, 4);
  little(wav, 32, 4, 2);
  little(wav, 34, 16, 2);
  text(36, "data");
  little(wav, 40, static_cast<std::uint32_t>(pcm.size()), 4);
  for (std::size_t i = 0; i < pcm.size(); i += 2) {
    wav[44 + i] = pcm[i + 1];
    wav[44 + i + 1] = pcm[i];
  }
  return wav;
}
Bytes decode_wav(std::span<const std::uint8_t> wav, int rate) {
  if (wav.size() < 44 || !tag(wav, 0, "RIFF") || !tag(wav, 8, "WAVE"))
    throw std::runtime_error("Expected a RIFF WAV file");
  bool format = false;
  Bytes pcm;
  for (std::size_t p = 12; p + 8 <= wav.size();) {
    const std::size_t size = le32(wav, p + 4), start = p + 8;
    if (size > wav.size() - start)
      throw std::runtime_error("Truncated WAV chunk");
    if (tag(wav, p, "fmt ") && size >= 16)
      format = le16(wav, start) == 1 && le16(wav, start + 2) == 2 &&
               le32(wav, start + 4) == static_cast<std::uint32_t>(rate) &&
               le16(wav, start + 14) == 16 && le16(wav, start + 12) == 4;
    if (tag(wav, p, "data"))
      pcm.assign(wav.begin() + start, wav.begin() + start + size);
    p = start + size + (size % 2);
  }
  if (!format || pcm.empty() || pcm.size() % 4)
    throw std::runtime_error("WAV must be PCM 16-bit, stereo, at the configured sample rate");
  for (std::size_t i = 0; i < pcm.size(); i += 2)
    std::swap(pcm[i], pcm[i + 1]);
  return pcm;
}
Pcm wav_packet(const Bytes& source, std::uint64_t offset_frames) {
  if (source.empty() || source.size() % 4)
    throw std::runtime_error("Invalid WAV source");
  Pcm result;
  const auto offset = (offset_frames % (source.size() / 4)) * 4;
  for (std::size_t i = 0; i < result.size(); ++i)
    result[i] = source[(offset + i) % source.size()];
  return result;
}
Bytes load_wav(const std::string& path, int rate) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file || file.tellg() > 100 * 1024 * 1024 || file.tellg() < 0)
    throw std::runtime_error("Cannot read WAV (100 MiB maximum): " + path);
  const auto size = static_cast<std::size_t>(file.tellg());
  file.seekg(0);
  Bytes data(size);
  if (!file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size)))
    throw std::runtime_error("WAV read failed");
  return decode_wav(data, rate);
}
JitterBuffer::JitterBuffer(int latency, int rate) : latency_ms_(latency), sample_rate_(rate) {
  packet_duration_ms(rate);
  if (latency < 20 || latency > 500)
    throw std::runtime_error("Buffer must be 20-500 ms");
}
void JitterBuffer::reset() {
  queue_.clear();
  expected_.reset();
  last_sequence_.reset();
  last_arrival_.reset();
  playing_ = false;
}
void JitterBuffer::push(const Packet& p, double now) {
  if (last_arrival_ && now - *last_arrival_ > 1000) {
    reset();
    ++stats.resets;
  }
  if (!expected_) {
    expected_ = p.sequence;
    start_at_ = now + latency_ms_;
  }
  const int distance = sequence_distance(p.sequence, *expected_);
  if (distance < 0) {
    ++stats.late;
    return;
  }
  if (distance >
      std::max(200, static_cast<int>(latency_ms_ / packet_duration_ms(sample_rate_)) + 40)) {
    reset();
    ++stats.resets;
    expected_ = p.sequence;
    start_at_ = now + latency_ms_;
  }
  if (queue_.contains(p.sequence)) {
    ++stats.duplicates;
    return;
  }
  if (last_sequence_ && sequence_distance(p.sequence, *last_sequence_) < 0)
    ++stats.reordered;
  if (last_arrival_) {
    const auto raw = p.timestamp - last_timestamp_;
    const double delta = raw <= 0x7fffffff ? double(raw) : double(raw) - 4294967296.0;
    const double difference = std::abs((now - *last_arrival_) - delta * 1000 / sample_rate_);
    stats.jitter_ms += (difference - stats.jitter_ms) / 16;
  }
  last_sequence_ = p.sequence;
  last_arrival_ = now;
  last_timestamp_ = p.timestamp;
  ++stats.received;
  queue_.emplace(p.sequence, p.pcm);
}
std::optional<Pcm> JitterBuffer::take(double now) {
  if (!expected_ || now < start_at_)
    return std::nullopt;
  if (last_arrival_ && now - *last_arrival_ > 1000) {
    reset();
    ++stats.resets;
    return std::nullopt;
  }
  playing_ = true;
  const auto found = queue_.find(*expected_);
  *expected_ = std::uint16_t(*expected_ + 1);
  if (found == queue_.end()) {
    ++stats.lost;
    return Pcm{};
  }
  Pcm pcm = found->second;
  queue_.erase(found);
  return pcm;
}
void AudioRing::push(const Pcm& pcm) {
  blocks_.push_back(pcm);
  if (blocks_.size() > static_cast<std::size_t>(rate_ * 10 / frames_per_packet))
    blocks_.pop_front();
}
Bytes AudioRing::snapshot() const {
  Bytes result;
  result.reserve(blocks_.size() * payload_bytes);
  for (const auto& b : blocks_)
    result.insert(result.end(), b.begin(), b.end());
  return result;
}
} // namespace aoip
