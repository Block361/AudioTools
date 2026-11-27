#pragma once
#include "aoip/audio.hpp"
#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace aoip {
enum class AudioBackend { System, TestNull, Wasapi, Alsa };
struct AudioDeviceInfo {
  int index{};
  std::string name;
  bool is_default{};
  std::string id;
};
struct AudioDevices {
  std::string backend;
  std::vector<AudioDeviceInfo> capture, playback;
};
AudioDevices audio_devices(AudioBackend backend = AudioBackend::System);

// Frame-aligned, single-producer/single-consumer queue. The callback never allocates or locks.
class AudioFifo {
public:
  AudioFifo(std::size_t channels, std::size_t capacity_frames);
  std::size_t write(std::span<const float> samples);
  std::size_t read(std::span<float> samples);
  std::size_t available() const;
  std::size_t channels() const {
    return channels_;
  }

private:
  std::size_t channels_, capacity_;
  std::vector<float> samples_;
  alignas(64) std::atomic<std::uint64_t> read_{0};
  alignas(64) std::atomic<std::uint64_t> write_{0};
};

// Maps are 1-based physical channel numbers, two entries per network stereo stream.
// A mono capture is duplicated to L/R by default. Other defaults use consecutive pairs.
std::vector<int> channel_map(const std::vector<int>& requested, int streams, int device_channels,
                             bool capture);
void capture_to_packets(std::span<const float> frames, int channels, std::span<const int> map,
                        std::span<Pcm> packets);
void packets_to_playback(std::span<const Pcm> packets, std::span<const int> map, int channels,
                         float gain, std::span<float> output);
struct AudioIoStatus {
  std::string device, backend, state;
  int channels{};
  std::vector<int> map;
  std::uint64_t frames{}, dropped_frames{}, underrun_frames{}, drift_corrections{};
};
class CaptureDevice {
public:
  CaptureDevice(const std::string& selector, int streams, const std::vector<int>& map = {},
                AudioBackend backend = AudioBackend::System, int rate = sample_rate);
  ~CaptureDevice();
  void start();
  bool read(std::span<Pcm> packets);
  AudioIoStatus status() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
class PlaybackDevice {
public:
  PlaybackDevice(const std::string& selector, int inputs, const std::vector<int>& map = {},
                 float gain = 0.5f, AudioBackend backend = AudioBackend::System,
                 int rate = sample_rate);
  ~PlaybackDevice();
  void start();
  void write(std::span<const Pcm> packets);
  AudioIoStatus status() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace aoip
