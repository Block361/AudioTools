#include "aoip/audio_io.hpp"
#include "miniaudio.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

namespace aoip {
AudioFifo::AudioFifo(std::size_t channels, std::size_t frames)
    : channels_(channels), capacity_(frames) {
  if (!channels || channels > 32 || !frames || frames > 48000)
    throw std::runtime_error("Invalid audio FIFO dimensions");
  samples_.resize(channels * frames);
}
std::size_t AudioFifo::write(std::span<const float> input) {
  if (input.size() % channels_)
    throw std::runtime_error("Unaligned audio frames");
  const auto position = write_.load(std::memory_order_relaxed);
  const auto read = read_.load(std::memory_order_acquire);
  const auto frames = std::min(input.size() / channels_, capacity_ - std::size_t(position - read));
  for (std::size_t f = 0; f < frames; ++f)
    std::copy_n(input.data() + f * channels_, channels_,
                samples_.data() + ((position + f) % capacity_) * channels_);
  write_.store(position + frames, std::memory_order_release);
  return frames;
}
std::size_t AudioFifo::read(std::span<float> output) {
  if (output.size() % channels_)
    throw std::runtime_error("Unaligned audio frames");
  const auto position = read_.load(std::memory_order_relaxed);
  const auto write = write_.load(std::memory_order_acquire);
  const auto frames = std::min(output.size() / channels_, std::size_t(write - position));
  for (std::size_t f = 0; f < frames; ++f)
    std::copy_n(samples_.data() + ((position + f) % capacity_) * channels_, channels_,
                output.data() + f * channels_);
  read_.store(position + frames, std::memory_order_release);
  return frames;
}
std::size_t AudioFifo::available() const {
  const auto read = read_.load(std::memory_order_acquire);
  const auto write = write_.load(std::memory_order_acquire);
  return std::min(capacity_, std::size_t(write - read));
}
std::vector<int> channel_map(const std::vector<int>& requested, int streams, int channels,
                             bool capture) {
  if (streams < 1 || streams > 16 || channels < 1 || channels > 32)
    throw std::runtime_error("Expected 1-16 stereo streams and 1-32 hardware channels");
  auto map = requested;
  if (map.empty()) {
    for (int i = 0; i < streams * 2; ++i)
      map.push_back(capture && channels == 1 && streams == 1 ? 1 : i + 1);
  }
  if (map.size() != static_cast<std::size_t>(streams * 2))
    throw std::runtime_error("Channel map needs two physical channel numbers per stereo stream");
  for (int channel : map)
    if (channel < 1 || channel > channels)
      throw std::runtime_error("Channel map exceeds the sound card's " + std::to_string(channels) +
                               " channels; reduce streams/inputs or specify a channel map");
  if (!capture) {
    auto sorted = map;
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
      throw std::runtime_error(
          "Each playback channel can be assigned only once; this prototype does not mix inputs");
  }
  return map;
}
void capture_to_packets(std::span<const float> frames, int channels, std::span<const int> map,
                        std::span<Pcm> packets) {
  if (channels < 1 || frames.size() != static_cast<std::size_t>(channels * frames_per_packet) ||
      map.size() != packets.size() * 2)
    throw std::runtime_error("Invalid capture block dimensions");
  for (std::size_t stream = 0; stream < packets.size(); ++stream)
    for (int frame = 0; frame < frames_per_packet; ++frame)
      for (int side = 0; side < 2; ++side) {
        const auto channel = map[stream * 2 + side] - 1;
        if (channel < 0 || channel >= channels)
          throw std::runtime_error("Invalid capture channel");
        const float value = frames[frame * channels + channel];
        const auto sample = static_cast<std::uint16_t>(static_cast<std::int16_t>(
            std::lround(std::clamp(std::isfinite(value) ? value : 0.0f, -1.0f, 1.0f) * 32767)));
        packets[stream][frame * 4 + side * 2] = static_cast<std::uint8_t>(sample >> 8);
        packets[stream][frame * 4 + side * 2 + 1] = static_cast<std::uint8_t>(sample);
      }
}
void packets_to_playback(std::span<const Pcm> packets, std::span<const int> map, int channels,
                         float gain, std::span<float> output) {
  if (channels < 1 || output.size() != static_cast<std::size_t>(channels * frames_per_packet) ||
      map.size() != packets.size() * 2)
    throw std::runtime_error("Invalid playback block dimensions");
  std::fill(output.begin(), output.end(), 0.0f);
  for (std::size_t stream = 0; stream < packets.size(); ++stream)
    for (int frame = 0; frame < frames_per_packet; ++frame)
      for (int side = 0; side < 2; ++side) {
        const int channel = map[stream * 2 + side] - 1;
        if (channel < 0 || channel >= channels)
          throw std::runtime_error("Invalid playback channel");
        const int offset = frame * 4 + side * 2;
        const int raw = (int(packets[stream][offset]) << 8) | packets[stream][offset + 1];
        output[frame * channels + channel] = (raw >= 32768 ? raw - 65536 : raw) / 32768.0f * gain;
      }
}
namespace {
void check(ma_result result, const std::string& action) {
  if (result != MA_SUCCESS)
    throw std::runtime_error(action + ": " + ma_result_description(result));
}
struct Context {
  ma_context context{};
  explicit Context(AudioBackend backend) {
    const ma_backend selected = backend == AudioBackend::TestNull ? ma_backend_null
                               : backend == AudioBackend::Alsa ? ma_backend_alsa : ma_backend_wasapi;
    check(ma_context_init(backend != AudioBackend::System ? &selected : nullptr,
                          backend != AudioBackend::System ? 1 : 0, nullptr, &context),
          "Initialize audio backend");
    if (backend != AudioBackend::TestNull && context.backend == ma_backend_null) {
      ma_context_uninit(&context);
      throw std::runtime_error("No native audio backend available");
    }
  }
  ~Context() {
    ma_context_uninit(&context);
  }
};
std::optional<ma_device_id> select(Context& context, const std::string& selector, bool capture) {
  if (selector == "default")
    return std::nullopt;
  ma_device_info *outputs{}, *inputs{};
  ma_uint32 output_count{}, input_count{};
  check(ma_context_get_devices(&context.context, &outputs, &output_count, &inputs, &input_count),
        "List sound cards");
  const auto count = capture ? input_count : output_count;
  const auto info = capture ? inputs : outputs;
  if (!selector.empty() && std::all_of(selector.begin(), selector.end(),
                                       [](unsigned char c) { return c >= '0' && c <= '9'; })) {
    const auto index = std::stoull(selector);
    if (index >= count)
      throw std::runtime_error("Audio device index is unavailable; run openaoip audio-devices");
    return info[index].id;
  }
  std::optional<ma_device_id> found;
  for (ma_uint32 i = 0; i < count; ++i)
    if (selector == info[i].name || (context.context.backend==ma_backend_alsa && selector==std::string("alsa:")+info[i].id.alsa)) {
      if (found)
        throw std::runtime_error("Device name is ambiguous; use its audio-devices index");
      found = info[i].id;
    }
  if (!found)
    throw std::runtime_error("Audio device not found: " + selector +
                             "; run openaoip audio-devices");
  return found;
}
struct NativeDevice {
  Context context;
  ma_device device{};
  bool initialized = false;
  explicit NativeDevice(AudioBackend backend) : context(backend) {}
  ~NativeDevice() {
    if (initialized)
      ma_device_uninit(&device);
  }
  void init(ma_device_config& config) {
    check(ma_device_init(&context.context, &config, &device),
          config.deviceType == ma_device_type_capture
              ? "Open microphone (check OS microphone permissions and sandbox restrictions)"
              : "Open sound card output");
    initialized = true;
  }
  void start() {
    check(ma_device_start(&device),
          "Start sound card (check device availability and microphone permissions)");
  }
  std::string state() const {
    return ma_device_get_state(&device) == ma_device_state_started ? "running" : "stopped";
  }
};
} // namespace
AudioDevices audio_devices(AudioBackend backend) {
  Context context(backend);
  ma_device_info *outputs{}, *inputs{};
  ma_uint32 output_count{}, input_count{};
  check(ma_context_get_devices(&context.context, &outputs, &output_count, &inputs, &input_count),
        "List audio devices");
  AudioDevices result;
  result.backend = ma_get_backend_name(context.context.backend);
  auto identifier = [&](const ma_device_info& info) {
    if (context.context.backend == ma_backend_alsa) return std::string("alsa:") + info.id.alsa;
#ifdef _WIN32
    if(context.context.backend==ma_backend_wasapi){const auto* raw=reinterpret_cast<const wchar_t*>(info.id.wasapi);const int n=WideCharToMultiByte(CP_UTF8,0,raw,-1,nullptr,0,nullptr,nullptr);std::string id(std::max(0,n),0);if(n)WideCharToMultiByte(CP_UTF8,0,raw,-1,id.data(),n,nullptr,nullptr);if(!id.empty())id.pop_back();return "id:"+id;}
#endif
    return std::string(info.name);
  };
  for (ma_uint32 i = 0; i < input_count; ++i)
    result.capture.push_back({int(i), inputs[i].name, inputs[i].isDefault != 0,identifier(inputs[i])});
  for (ma_uint32 i = 0; i < output_count; ++i)
    result.playback.push_back({int(i), outputs[i].name, outputs[i].isDefault != 0,identifier(outputs[i])});
  return result;
}
struct CaptureDevice::Impl {
  // NativeDevice is declared last so callbacks are stopped before callback-owned storage is freed.
  std::unique_ptr<AudioFifo> fifo;
  std::vector<int> map;
  std::vector<float> block;
  std::atomic<std::uint64_t> frames{0}, dropped{0};
  NativeDevice native;
  Impl(const std::string& selector, int streams, const std::vector<int>& requested,
       AudioBackend backend, int rate)
      : native(backend) {
    const auto id = select(native.context, selector, true);
    auto config = ma_device_config_init(ma_device_type_capture);
    config.capture.pDeviceID = id ? &*id : nullptr;
    config.capture.format = ma_format_f32;
    config.capture.channels = 0;
    config.sampleRate = rate;
    config.periodSizeInFrames = 480;
    config.dataCallback = [](ma_device* device, void*, const void* input, ma_uint32 count) {
      auto& self = *static_cast<Impl*>(device->pUserData);
      if (!input)
        return;
      const auto written = self.fifo->write(
          {static_cast<const float*>(input), std::size_t(count) * device->capture.channels});
      self.frames.fetch_add(count, std::memory_order_relaxed);
      self.dropped.fetch_add(count - written, std::memory_order_relaxed);
    };
    config.pUserData = this;
    native.init(config);
    map = channel_map(requested, streams, static_cast<int>(native.device.capture.channels), true);
    fifo = std::make_unique<AudioFifo>(native.device.capture.channels, 9600);
    block.resize(native.device.capture.channels * frames_per_packet);
  }
};
CaptureDevice::CaptureDevice(const std::string& selector, int streams, const std::vector<int>& map,
                             AudioBackend backend, int rate)
    : impl_(std::make_unique<Impl>(selector, streams, map, backend, rate)) {}
CaptureDevice::~CaptureDevice() = default;
void CaptureDevice::start() {
  impl_->native.start();
}
bool CaptureDevice::read(std::span<Pcm> packets) {
  if (impl_->fifo->available() < frames_per_packet)
    return false;
  impl_->fifo->read(impl_->block);
  capture_to_packets(impl_->block, static_cast<int>(impl_->native.device.capture.channels),
                     impl_->map, packets);
  return true;
}
AudioIoStatus CaptureDevice::status() const {
  const auto& s = *impl_;
  return {s.native.device.capture.name,
          ma_get_backend_name(s.native.context.context.backend),
          s.native.state(),
          static_cast<int>(s.native.device.capture.channels),
          s.map,
          s.frames.load(),
          s.dropped.load(),
          0,
          0};
}
struct PlaybackDevice::Impl {
  std::unique_ptr<AudioFifo> fifo;
  std::vector<int> map;
  std::vector<float> block, adjusted;
  float gain;
  std::atomic<std::uint64_t> frames{0}, dropped{0}, underruns{0}, corrections{0};
  NativeDevice native;
  Impl(const std::string& selector, int streams, const std::vector<int>& requested, float volume,
       AudioBackend backend, int rate)
      : gain(volume), native(backend) {
    if (!std::isfinite(gain) || gain < 0 || gain > 1)
      throw std::runtime_error("Playback gain must be 0-1");
    const auto id = select(native.context, selector, false);
    auto config = ma_device_config_init(ma_device_type_playback);
    config.playback.pDeviceID = id ? &*id : nullptr;
    config.playback.format = ma_format_f32;
    config.playback.channels = 0;
    config.sampleRate = rate;
    config.periodSizeInFrames = 480;
    config.dataCallback = [](ma_device* device, void* output, const void*, ma_uint32 count) {
      auto& self = *static_cast<Impl*>(device->pUserData);
      auto samples =
          std::span(static_cast<float*>(output), std::size_t(count) * device->playback.channels);
      const auto read = self.fifo->read(samples);
      std::fill(samples.begin() + read * device->playback.channels, samples.end(), 0.0f);
      self.frames.fetch_add(count, std::memory_order_relaxed);
      self.underruns.fetch_add(count - read, std::memory_order_relaxed);
    };
    config.pUserData = this;
    native.init(config);
    map = channel_map(requested, streams, static_cast<int>(native.device.playback.channels), false);
    fifo = std::make_unique<AudioFifo>(native.device.playback.channels, 9600);
    block.resize(native.device.playback.channels * frames_per_packet);
    adjusted.resize(native.device.playback.channels * (frames_per_packet + 1));
    // Prime with 20 ms of silence before starting the device clock.
    std::vector<float> silence(native.device.playback.channels * (rate / 50), 0.0f);
    fifo->write(silence);
  }
};
PlaybackDevice::PlaybackDevice(const std::string& selector, int streams,
                               const std::vector<int>& map, float gain, AudioBackend backend,
                               int rate)
    : impl_(std::make_unique<Impl>(selector, streams, map, gain, backend, rate)) {}
PlaybackDevice::~PlaybackDevice() = default;
void PlaybackDevice::start() {
  impl_->native.start();
}
void PlaybackDevice::write(std::span<const Pcm> packets) {
  auto& s = *impl_;
  const int channels = static_cast<int>(s.native.device.playback.channels);
  packets_to_playback(packets, s.map, channels, s.gain, s.block);
  // Bounded one-frame interpolation keeps the hardware queue near 20 ms despite clock drift.
  // This is a prototype queue servo, not PTP or a phase-coherent sample clock.
  const auto queued = s.fifo->available();
  const int frames = queued < s.native.device.sampleRate * .015   ? 241
                     : queued > s.native.device.sampleRate * .025 ? 239
                                                                  : 240;
  std::span<const float> output = s.block;
  if (frames != frames_per_packet) {
    for (int f = 0; f < frames; ++f) {
      const double position = f * double(frames_per_packet - 1) / (frames - 1);
      const int left = static_cast<int>(position),
                right = std::min(left + 1, frames_per_packet - 1);
      const float fraction = static_cast<float>(position - left);
      for (int c = 0; c < channels; ++c)
        s.adjusted[f * channels + c] = s.block[left * channels + c] * (1 - fraction) +
                                       s.block[right * channels + c] * fraction;
    }
    output = std::span(s.adjusted).first(static_cast<std::size_t>(frames * channels));
    s.corrections.fetch_add(1, std::memory_order_relaxed);
  }
  const auto written = s.fifo->write(output);
  s.dropped.fetch_add(frames - written, std::memory_order_relaxed);
}
AudioIoStatus PlaybackDevice::status() const {
  const auto& s = *impl_;
  return {s.native.device.playback.name,
          ma_get_backend_name(s.native.context.context.backend),
          s.native.state(),
          static_cast<int>(s.native.device.playback.channels),
          s.map,
          s.frames.load(),
          s.dropped.load(),
          s.underruns.load(),
          s.corrections.load()};
}
} // namespace aoip
