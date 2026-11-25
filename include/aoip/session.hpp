#pragma once
#include "aoip/system.hpp"
namespace aoip {
struct SessionConfig {
  std::string id, name, controller = "auto", interface_ip = "0.0.0.0", kind = "audio";
  int tx_channels = 8, rx_channels = 8, rate = 48000, latency_ms = 60;
  std::vector<std::string> tx_names, rx_names;
  std::function<bool(std::span<Pcm>)> capture;
  std::function<void(std::span<const Pcm>)> playback;
};
// Shared discovery, network-node lifecycle and diagnostics; independent of any Windows UI.
class AudioSession {
public:
  explicit AudioSession(SessionConfig);
  ~AudioSession();
  Json status() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
void pcm_to_float(std::span<const Pcm>, std::span<float>, int channels);
void float_to_pcm(std::span<const float>, std::span<Pcm>, int channels);
} // namespace aoip
