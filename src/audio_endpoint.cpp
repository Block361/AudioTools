#include "aoip/audio_endpoint.hpp"
#include "miniaudio.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif
namespace aoip {
namespace {
void require(ma_result result, const char* action) {
  if (result != MA_SUCCESS)
    throw std::runtime_error(std::string(action) + ": " + ma_result_description(result));
}
struct Host {
  ma_context context{};
  std::mutex control;
  explicit Host(AudioBackend backend) {
    const ma_backend selected = backend == AudioBackend::TestNull ? ma_backend_null
                                : backend == AudioBackend::Alsa   ? ma_backend_alsa
                                : backend == AudioBackend::Wasapi ? ma_backend_wasapi
#ifdef _WIN32
                                                                  : ma_backend_wasapi;
#else
                                                                  : ma_backend_alsa;
#endif
    require(ma_context_init(&selected, 1, nullptr, &context), "Initialize native audio backend");
  }
  ~Host() {
    ma_context_uninit(&context);
  }
};
std::shared_ptr<Host> get_host(AudioBackend backend) {
  static std::mutex mutex;
  static std::map<AudioBackend, std::weak_ptr<Host>> hosts;
  std::lock_guard lock(mutex);
  auto& slot = hosts[backend];
  auto host = slot.lock();
  if (!host) {
    host = std::make_shared<Host>(backend);
    slot = host;
  }
  return host;
}
std::optional<ma_device_id> select_endpoint(Host& host, const EndpointConfig& config) {
  if (config.device == "default")
    return std::nullopt;
  ma_device_info *playback = nullptr, *capture = nullptr;
  ma_uint32 np = 0, nc = 0;
  require(ma_context_get_devices(&host.context, &playback, &np, &capture, &nc),
          "Enumerate native audio devices");
  const bool input = config.mode == EndpointMode::Capture;
  const auto* items = input ? capture : playback;
  const auto count = input ? nc : np;
  for (ma_uint32 i = 0; i < count; ++i) {
    bool match = config.device == items[i].name;
    if (host.context.backend == ma_backend_alsa && config.device.starts_with("alsa:"))
      match = config.device.substr(5) == items[i].id.alsa;
#ifdef _WIN32
    if (config.device.starts_with("id:") && host.context.backend == ma_backend_wasapi) {
      const auto& id = config.device;
      const int n = MultiByteToWideChar(CP_UTF8, 0, id.data() + 3, static_cast<int>(id.size() - 3),
                                        nullptr, 0);
      std::wstring wide(n, 0);
      MultiByteToWideChar(CP_UTF8, 0, id.data() + 3, static_cast<int>(id.size() - 3), wide.data(),
                          n);
      match = wide == reinterpret_cast<const wchar_t*>(items[i].id.wasapi);
    }
#endif
    if (match)
      return items[i].id;
  }
  throw std::runtime_error(
      "Selected audio device is unavailable; waiting for the same device to return");
}
} // namespace
struct AudioEndpoint::Impl {
  std::shared_ptr<Host> host;
  EndpointConfig config;
  ma_device device{};
  bool initialized = false;
  std::atomic<bool> online{false};
  Impl(EndpointConfig c, AudioBackend backend) : host(get_host(backend)), config(std::move(c)) {
    if (config.mode == EndpointMode::Loopback && host->context.backend != ma_backend_wasapi)
      throw std::runtime_error("This backend has no system-mix loopback. Use Capture with an ALSA "
                               "loopback/monitor device.");
    if (!supported_sample_rate(config.sample_rate) || config.buffer_frames < 64 ||
        config.buffer_frames > 2048)
      throw std::runtime_error("Invalid audio format or buffer size");
    std::lock_guard lock(host->control);
    const auto id = select_endpoint(*host, config);
    auto native =
        ma_device_config_init(config.mode == EndpointMode::Playback   ? ma_device_type_playback
                              : config.mode == EndpointMode::Loopback ? ma_device_type_loopback
                                                                      : ma_device_type_capture);
    native.sampleRate = config.sample_rate;
    native.periodSizeInFrames = config.buffer_frames;
    native.resampling.algorithm = ma_resample_algorithm_linear;
    native.resampling.linear.lpfOrder = 4;
    native.wasapi.noAutoConvertSRC = MA_TRUE;
    native.pUserData = this;
    if (config.mode == EndpointMode::Playback) {
      native.playback.pDeviceID = id ? &*id : nullptr;
      native.playback.format = ma_format_f32;
      native.playback.channels = 0;
    } else {
      native.capture.pDeviceID = id ? &*id : nullptr;
      native.capture.format = ma_format_f32;
      native.capture.channels = 0;
    }
    native.dataCallback = [](ma_device* device, void* output, const void* input, ma_uint32 frames) {
      auto& self = *static_cast<Impl*>(device->pUserData);
      if (self.config.mode == EndpointMode::Playback) {
        auto data =
            std::span(static_cast<float*>(output), std::size_t(frames) * device->playback.channels);
        std::fill(data.begin(), data.end(), 0.f);
        if (self.config.render)
          self.config.render(data);
      } else if (input && self.config.capture)
        self.config.capture(
            {static_cast<const float*>(input), std::size_t(frames) * device->capture.channels});
    };
    native.notificationCallback = [](const ma_device_notification* notice) {
      auto& self = *static_cast<Impl*>(notice->pDevice->pUserData);
      if (notice->type == ma_device_notification_type_started)
        self.online = true;
      else if (notice->type == ma_device_notification_type_stopped ||
               notice->type == ma_device_notification_type_rerouted ||
               notice->type == ma_device_notification_type_interruption_began)
        self.online = false;
    };
    require(ma_device_init(&host->context, &native, &device), "Open native audio endpoint");
    initialized = true;
    if (channels() < 1 || channels() > 32) {
      ma_device_uninit(&device);
      initialized = false;
      throw std::runtime_error("The selected audio endpoint must expose 1-32 channels");
    }
  }
  int channels() const {
    return static_cast<int>(config.mode == EndpointMode::Playback ? device.playback.channels
                                                                  : device.capture.channels);
  }
  ~Impl() {
    if (initialized)
      ma_device_uninit(&device);
  }
};
AudioEndpoint::AudioEndpoint(EndpointConfig c, AudioBackend b)
    : impl_(std::make_unique<Impl>(std::move(c), b)) {}
AudioEndpoint::~AudioEndpoint() = default;
void AudioEndpoint::start() {
  require(ma_device_start(&impl_->device),
          "Start audio endpoint (check device access and audio permissions)");
  impl_->online = true;
}
void AudioEndpoint::stop() {
  if (impl_->initialized)
    ma_device_stop(&impl_->device);
  impl_->online = false;
}
bool AudioEndpoint::connected() const {
  return impl_->online.load() && ma_device_get_state(&impl_->device) == ma_device_state_started;
}
int AudioEndpoint::channels() const {
  return impl_->channels();
}
int AudioEndpoint::native_rate() const {
  return static_cast<int>(impl_->config.mode == EndpointMode::Playback
                              ? impl_->device.playback.internalSampleRate
                              : impl_->device.capture.internalSampleRate);
}
int AudioEndpoint::native_buffer_frames() const {
  return static_cast<int>(impl_->config.mode == EndpointMode::Playback
                              ? impl_->device.playback.internalPeriodSizeInFrames
                              : impl_->device.capture.internalPeriodSizeInFrames);
}
std::string AudioEndpoint::name() const {
  return impl_->config.mode == EndpointMode::Playback ? impl_->device.playback.name
                                                      : impl_->device.capture.name;
}
} // namespace aoip
