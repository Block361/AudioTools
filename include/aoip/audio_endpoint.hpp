#pragma once
#include "aoip/audio_io.hpp"
#include <functional>
namespace aoip {
enum class EndpointMode { Capture, Loopback, Playback };
struct EndpointConfig {
  EndpointMode mode = EndpointMode::Capture;
  std::string device = "default";
  int sample_rate = 48000, buffer_frames = 256;
  std::function<void(std::span<const float>)> capture;
  std::function<void(std::span<float>)> render;
};
// Native user-mode endpoint. No kernel driver or ASIO SDK dependency.
class AudioEndpoint {
public:
  AudioEndpoint(EndpointConfig, AudioBackend backend = AudioBackend::Wasapi);
  ~AudioEndpoint();
  void start();
  void stop();
  bool connected() const;
  int channels() const;
  int native_rate() const;
  int native_buffer_frames() const;
  std::string name() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace aoip
