#include "aoip/bridge_engine.hpp"
#include "aoip/adaptive_buffer.hpp"
#include "aoip/network.hpp"
#include <array>
#include <cmath>
#include <mutex>
#include <set>
#include <thread>
namespace aoip::bridge {
namespace {
std::string mode_name(EndpointMode m) {
  return m == EndpointMode::Capture    ? "capture"
         : m == EndpointMode::Loopback ? "loopback"
                                       : "playback";
}
struct Meter {
  std::array<std::atomic<float>, 32> peaks{};
  void update(std::span<const float> samples, int channels) {
    std::array<float, 32> block{};
    for (std::size_t i = 0; i < samples.size(); ++i)
      block[i % channels] = std::max(block[i % channels], std::abs(samples[i]));
    for (int c = 0; c < channels; ++c)
      peaks[c].store(std::max(block[c], peaks[c].load(std::memory_order_relaxed) * .85f),
                     std::memory_order_relaxed);
  }
  Json read(int channels) const {
    auto out = Json::array();
    for (int c = 0; c < channels; ++c)
      out.push_back(peaks[c].load());
    return out;
  }
};
struct Runtime {
  std::unique_ptr<AdaptiveBuffer> buffer;
  std::vector<float> block;
  Meter meter;
  int channels = 0;
  std::unique_ptr<AudioEndpoint> audio; // Destroy native callbacks before their buffers.
  Runtime(const Endpoint& endpoint, const Settings& s, AudioBackend backend) {
    EndpointConfig c;
    c.mode = endpoint.mode;
    c.device = endpoint.device;
    c.sample_rate = s.rate;
    c.buffer_frames = s.buffer_frames;
    c.capture = [this](std::span<const float> data) {
      buffer->write(data);
      meter.update(data, channels);
    };
    c.render = [this](std::span<float> data) {
      buffer->read(data);
      meter.update(data, channels);
    };
    audio = std::make_unique<AudioEndpoint>(std::move(c), backend);
    channels = audio->channels();
    for (const auto& patch : endpoint.patches)
      if (patch.local > channels)
        throw std::runtime_error("Local channel " + std::to_string(patch.local) +
                                 " exceeds the device's " + std::to_string(channels) + " channels");
    buffer = std::make_unique<AdaptiveBuffer>(channels, s.rate, s.local_latency_ms);
    block.resize(frames_per_packet * channels);
    audio->start();
  }
};
struct Node {
  Endpoint config;
  std::atomic<std::shared_ptr<Runtime>> runtime;
  mutable std::mutex mutex;
  std::string error;
  std::uint64_t reconnects = 0;
  double retry_at = 0;
};
} // namespace
Json to_json(const Settings& s) {
  Json endpoints = Json::array();
  for (const auto& e : s.endpoints) {
    Json maps = Json::array();
    for (const auto& p : e.patches)
      maps.push_back({{"local", p.local}, {"network", p.network}, {"gain", p.gain}});
    endpoints.push_back({{"id", e.id},
                         {"name", e.name},
                         {"mode", mode_name(e.mode)},
                         {"device", e.device},
                         {"patches", maps}});
  }
  return {{"id", s.id},
          {"name", s.name},
          {"controller", s.controller},
          {"interface", s.interface_ip},
          {"txChannels", s.tx_channels},
          {"rxChannels", s.rx_channels},
          {"sampleRate", s.rate},
          {"bufferFrames", s.buffer_frames},
          {"latencyMs", s.latency_ms},
          {"localLatencyMs", s.local_latency_ms},
          {"clockSource", s.clock_source},
          {"ptpDomain", s.ptp_domain},
          {"txNames", s.tx_names},
          {"rxNames", s.rx_names},
          {"endpoints", endpoints}};
}
Settings from_json(const Json& j) {
  Settings s;
  s.id = j.value("id", s.id);
  s.name = j.value("name", s.name);
  s.controller = j.value("controller", s.controller);
  s.interface_ip = j.value("interface", s.interface_ip);
  s.tx_channels = j.value("txChannels", 8);
  s.rx_channels = j.value("rxChannels", 8);
  s.rate = j.value("sampleRate", 48000);
  s.buffer_frames = j.value("bufferFrames", 256);
  s.latency_ms = j.value("latencyMs", 60);
  s.local_latency_ms = j.value("localLatencyMs", 20);
  s.clock_source = j.value("clockSource", "internal");
  s.ptp_domain = j.value("ptpDomain", 0);
  s.tx_names = j.value("txNames", std::vector<std::string>{});
  s.rx_names = j.value("rxNames", std::vector<std::string>{});
  for (const auto& value : j.value("endpoints", Json::array())) {
    Endpoint e;
    e.id = value.at("id");
    e.name = value.value("name", e.id);
    e.device = value.at("device");
    const auto mode = value.at("mode").get<std::string>();
    if (mode != "capture" && mode != "playback" && mode != "loopback")
      throw std::runtime_error("Unknown WASAPI endpoint mode");
    e.mode = mode == "capture"    ? EndpointMode::Capture
             : mode == "loopback" ? EndpointMode::Loopback
                                  : EndpointMode::Playback;
    for (const auto& p : value.at("patches"))
      e.patches.push_back({p.at("local"), p.at("network"), p.value("gain", 1.f)});
    s.endpoints.push_back(std::move(e));
  }
  return s;
}
void validate(const Settings& s, bool require_endpoints) {
  if (!valid_id(s.id) || s.name.empty() || s.name.size() > 80 || !valid_ipv4(s.interface_ip) ||
      (s.controller != "auto" &&
       (!s.controller.starts_with("http://") || s.controller.size() > 256)) ||
      s.latency_ms < 20 || s.latency_ms > 500 || s.ptp_domain < 0 || s.ptp_domain > 255)
    throw std::runtime_error(
        "Invalid bridge identity, controller, interface, latency or PTP domain");
  if (s.clock_source != "internal" && s.interface_ip == "0.0.0.0")
    throw std::runtime_error("PTP requires an explicit local IPv4 interface");
  for (const auto* names : {&s.tx_names, &s.rx_names}) {
    if (names->size() > 32)
      throw std::runtime_error("At most 32 channel names are allowed");
    for (const auto& name : *names)
      if (name.size() > 80)
        throw std::runtime_error("Channel name exceeds 80 bytes");
  }
  if (s.tx_channels < 1 || s.tx_channels > 32 || s.rx_channels < 1 || s.rx_channels > 32 ||
      !supported_sample_rate(s.rate) || (require_endpoints && s.endpoints.empty()) ||
      s.endpoints.size() > 32 || s.buffer_frames < 64 || s.buffer_frames > 2048 ||
      s.local_latency_ms < 5 || s.local_latency_ms > 200)
    throw std::runtime_error(
        "Bridge requires 1-32 channels, valid buffers and 1-32 local audio endpoints");
  if (s.clock_source != "internal" && s.clock_source != "ptp-monitor" &&
      s.clock_source != "ptp-grandmaster")
    throw std::runtime_error("Unknown clock source; select Internal, PTP follower or software GM");
  std::set<int> tx_mapped;
  std::set<std::string> ids, devices;
  for (const auto& e : s.endpoints) {
    if (!valid_id(e.id) || !ids.insert(e.id).second || e.device.empty() || e.patches.empty() ||
        e.patches.size() > 32)
      throw std::runtime_error("Invalid or duplicate local endpoint");
    if (!devices.insert(mode_name(e.mode) + ":" + e.device).second)
      throw std::runtime_error("Configure all channels of a local device in one endpoint entry");
    std::set<int> local;
    for (const auto& p : e.patches) {
      if (p.local < 1 || p.local > 32 || p.network < 1 ||
          p.network > (e.mode == EndpointMode::Playback ? s.rx_channels : s.tx_channels) ||
          !std::isfinite(p.gain) || p.gain < 0 || p.gain > 2)
        throw std::runtime_error("Local/network channel map is out of range");
      if (e.mode == EndpointMode::Playback) {
        if (!local.insert(p.local).second)
          throw std::runtime_error("A local output channel can be assigned only once");
      } else if (!tx_mapped.insert(p.network).second)
        throw std::runtime_error("Each network TX channel can have only one local source");
    }
  }
}
struct Engine::Impl {
  Settings settings;
  AudioBackend backend;
  std::vector<std::unique_ptr<Node>> nodes;
  std::vector<float> tx, rx;
  Meter tx_meter, rx_meter;
  std::atomic<bool> running{true};
  std::thread supervisor;
  std::unique_ptr<AudioSession> session;
  double deadline = 0;
  Impl(Settings s, AudioBackend b, std::shared_ptr<ptp::Monitor> clock, bool registration)
      : settings(std::move(s)), backend(b) {
    validate(settings);
    for (const auto& e : settings.endpoints) {
      auto node = std::make_unique<Node>();
      node->config = e;
      nodes.push_back(std::move(node));
    }
    tx.resize(settings.tx_channels * frames_per_packet);
    rx.resize(settings.rx_channels * frames_per_packet);
    SessionConfig c;
    c.id = settings.id;
    c.register_with_controller = registration;
    c.name = settings.name;
    c.kind = "bridge";
    c.controller = settings.controller;
    c.interface_ip = settings.interface_ip;
    c.tx_channels = settings.tx_channels;
    c.rx_channels = settings.rx_channels;
    c.rate = settings.rate;
    c.latency_ms = settings.latency_ms;
    c.tx_names = settings.tx_names;
    c.rx_names = settings.rx_names;
    c.clock_source = settings.clock_source;
    c.ptp_domain = settings.ptp_domain;
    c.ptp_clock = std::move(clock);
    c.capture = [this](std::span<Pcm> p) { return capture(p); };
    c.playback = [this](std::span<const Pcm> p) { playback(p); };
    session = std::make_unique<AudioSession>(std::move(c));
    try {
      supervisor = std::thread([this] { control(); });
    } catch (...) {
      session.reset();
      throw;
    }
  }
  ~Impl() {
    session.reset();
    running = false;
    if (supervisor.joinable())
      supervisor.join();
  }
  void control() {
    std::vector<std::shared_ptr<Runtime>> retired;
    while (running) {
      const auto now = monotonic_ms();
      for (auto& ptr : nodes) {
        auto& node = *ptr;
        auto active = node.runtime.load();
        if (active && !active->audio->connected()) {
          node.runtime.store(nullptr);
          active->audio->stop();
          retired.push_back(std::move(active));
          node.retry_at = now + 500;
        }
        if (!node.runtime.load() && now >= node.retry_at) {
          try {
            auto next = std::make_shared<Runtime>(node.config, settings, backend);
            node.runtime.store(next);
            std::lock_guard lock(node.mutex);
            node.error.clear();
            ++node.reconnects;
          } catch (const std::exception& e) {
            std::lock_guard lock(node.mutex);
            node.error = e.what();
            node.retry_at = now + 1000;
          }
        }
      }
      std::erase_if(retired, [](const auto& p) { return p.use_count() == 1; });
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    for (auto& n : nodes) {
      auto old = n->runtime.exchange(nullptr);
      if (old) {
        old->audio->stop();
        retired.push_back(std::move(old));
      }
    }
    retired.clear();
  }
  bool capture(std::span<Pcm> packets) {
    if (settings.clock_source == "internal") {
      const auto now = monotonic_ms(), interval = packet_duration_ms(settings.rate);
      if (now < deadline)
        return false;
      deadline = deadline && now - deadline < interval * 3 ? deadline + interval : now + interval;
    }
    std::fill(tx.begin(), tx.end(), 0.f);
    for (auto& node : nodes)
      if (node->config.mode != EndpointMode::Playback) {
        const auto state = node->runtime.load();
        if (!state)
          continue;
        state->buffer->read(state->block);
        for (const auto& p : node->config.patches)
          for (int frame = 0; frame < frames_per_packet; ++frame)
            tx[frame * settings.tx_channels + p.network - 1] =
                state->block[frame * state->channels + p.local - 1] * p.gain;
      }
    tx_meter.update(tx, settings.tx_channels);
    float_to_pcm(tx, packets, settings.tx_channels);
    return true;
  }
  void playback(std::span<const Pcm> packets) {
    pcm_to_float(packets, rx, settings.rx_channels);
    rx_meter.update(rx, settings.rx_channels);
    for (auto& node : nodes)
      if (node->config.mode == EndpointMode::Playback) {
        const auto state = node->runtime.load();
        if (!state)
          continue;
        std::fill(state->block.begin(), state->block.end(), 0.f);
        for (const auto& p : node->config.patches)
          for (int frame = 0; frame < frames_per_packet; ++frame)
            state->block[frame * state->channels + p.local - 1] =
                rx[frame * settings.rx_channels + p.network - 1] * p.gain;
        state->buffer->write(state->block);
      }
  }
  Json status() const {
    auto result = session->status();
    result["endpoints"] = Json::array();
    bool connected = true;
    std::uint64_t underruns = 0, dropped = 0;
    for (const auto& node : nodes) {
      Json value = {{"id", node->config.id},
                    {"name", node->config.name},
                    {"mode", mode_name(node->config.mode)},
                    {"device", node->config.device}};
      const auto state = node->runtime.load();
      {
        std::lock_guard lock(node->mutex);
        value["error"] = node->error;
        value["reconnects"] = node->reconnects;
      }
      value["connected"] = state && state->audio->connected();
      connected = connected && value.at("connected").get<bool>();
      if (state) {
        value["deviceName"] = state->audio->name();
        value["channels"] = state->channels;
        value["nativeSampleRate"] = state->audio->native_rate();
        value["nativeBufferFrames"] = state->audio->native_buffer_frames();
        value["queuedFrames"] = state->buffer->available();
        value["driftPpm"] = state->buffer->drift_ppm();
        value["underruns"] = state->buffer->underruns();
        value["dropped"] = state->buffer->dropped();
        value["peaks"] = state->meter.read(state->channels);
        underruns += state->buffer->underruns();
        dropped += state->buffer->dropped();
      }
      result["endpoints"].push_back(value);
    }
    result["audioConnected"] = connected;
    result["state"] = connected ? "Running" : "Waiting for audio device";
    result["txPeaks"] = tx_meter.read(settings.tx_channels);
    result["rxPeaks"] = rx_meter.read(settings.rx_channels);
    result["bufferUnderruns"] = underruns;
    result["audioDroppedFrames"] = dropped;
    result["bufferFrames"] = settings.buffer_frames;
    return result;
  }
};
Engine::Engine(Settings s, AudioBackend b, std::shared_ptr<ptp::Monitor> clock, bool registration)
    : impl_(std::make_unique<Impl>(std::move(s), b, std::move(clock), registration)) {}
Engine::~Engine() = default;
void Engine::apply_plan(const Json& plan) {
  impl_->session->apply_plan(plan);
}
Json Engine::status() const {
  return impl_->status();
}
} // namespace aoip::bridge
