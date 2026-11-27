#include "aoip/vsc_engine.hpp"
#ifdef _WIN32
#include "aoip/adaptive_buffer.hpp"
#include "aoip/network.hpp"
#include "aoip/session.hpp"
#include "aoip/virtual_bridge.hpp"
#include "aoip/wdm_client.hpp"
#include <array>
#include <atomic>
#include <cmath>
#include <thread>
namespace aoip::vsc {
Json to_json(const Settings& s) {
  return {{"id", s.id},
          {"name", s.name},
          {"interface", s.interface_ip},
          {"controller", s.controller},
          {"backend", s.backend},
          {"clockSource", s.clock_source},
          {"ptpDomain", s.ptp_domain},
          {"txChannels", s.tx_channels},
          {"rxChannels", s.rx_channels},
          {"sampleRate", s.rate},
          {"latencyMs", s.latency_ms},
          {"bufferFrames", s.buffer_frames},
          {"txNames", s.tx_names},
          {"rxNames", s.rx_names}};
}
Settings from_json(const Json& j) {
  Settings s;
  s.id = j.value("id", s.id);
  s.name = j.value("name", s.name);
  s.interface_ip = j.value("interface", s.interface_ip);
  s.controller = j.value("controller", s.controller);
  s.backend = j.value("backend", s.backend);
  s.clock_source = j.value("clockSource", "internal");
  s.ptp_domain = j.value("ptpDomain", 0);
  s.tx_channels = j.value("txChannels", 8);
  s.rx_channels = j.value("rxChannels", 8);
  s.rate = j.value("sampleRate", 48000);
  s.latency_ms = j.value("latencyMs", 60);
  s.buffer_frames = j.value("bufferFrames", 256);
  s.tx_names = j.value("txNames", std::vector<std::string>{});
  s.rx_names = j.value("rxNames", std::vector<std::string>{});
  return s;
}
struct Engine::Impl {
  Settings settings;
  BridgeMapping bridge;
  std::unique_ptr<WdmClient> wdm;
  std::vector<float> tx_block, rx_block;
  std::array<std::atomic<float>, 32> tx_peaks{}, rx_peaks{};
  std::atomic<bool> backend_failed{false};
  std::unique_ptr<AdaptiveBuffer> tx_buffer, rx_buffer;
  std::atomic<bool> pump_running{true};
  std::thread pump;
  std::unique_ptr<AudioSession> session;
  explicit Impl(Settings s, std::shared_ptr<ptp::Monitor> clock)
      : settings(std::move(s)), bridge(true, settings.bridge_mapping) {
    if (settings.tx_channels < 1 || settings.tx_channels > 32 || settings.rx_channels < 1 ||
        settings.rx_channels > 32 || !supported_sample_rate(settings.rate))
      throw std::runtime_error("Invalid virtual channel count or sample rate");
    if (settings.backend != "asio" && settings.backend != "wdm")
      throw std::runtime_error("Unknown Windows audio backend");
    if (settings.clock_source != "internal" && settings.clock_source != "ptp-monitor" &&
        settings.clock_source != "ptp-grandmaster")
      throw std::runtime_error(
          "Unknown clock source; select Internal, PTP follower or software GM");
    if (settings.buffer_frames < 64 || settings.buffer_frames > 1024 ||
        (settings.buffer_frames & (settings.buffer_frames - 1)))
      throw std::runtime_error("ASIO buffer must be 64/128/256/512/1024 frames");
    if (settings.backend == "wdm")
      wdm = std::make_unique<WdmClient>(settings.tx_channels, settings.rx_channels, settings.rate);
    tx_block.resize(frames_per_packet * settings.tx_channels);
    rx_block.resize(frames_per_packet * settings.rx_channels);
    if (settings.clock_source != "internal") {
      tx_buffer = std::make_unique<AdaptiveBuffer>(settings.tx_channels, settings.rate, 20);
      rx_buffer = std::make_unique<AdaptiveBuffer>(settings.rx_channels, settings.rate, 20);
    }
    bridge.configure(settings.tx_channels, settings.rx_channels, settings.rate, wdm ? 2 : 1);
    try {
      bridge.set_preferred_buffer(settings.buffer_frames);
      bridge.get()->latency_ms = settings.latency_ms;
      SessionConfig c;
      c.id = settings.id;
      c.name = settings.name;
      c.kind = "vsc";
      c.interface_ip = settings.interface_ip;
      c.controller = settings.controller;
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
      if (tx_buffer)
        pump = std::thread([this] { local_audio(); });
    } catch (...) {
      InterlockedExchange(&bridge.get()->active, 0);
      throw;
    }
  }
  ~Impl() {
    session.reset();
    pump_running = false;
    if (pump.joinable())
      pump.join();
    InterlockedExchange(&bridge.get()->active, 0);
  }
  bool read_local(std::span<float> block) {
    if (wdm)
      return wdm->capture(block);
    auto& ring = bridge.get()->to_network;
    if (ring_available(ring) < frames_per_packet)
      return false;
    ring_read(ring, block, settings.tx_channels);
    return true;
  }
  void write_local(std::span<const float> block) {
    if (wdm)
      wdm->playback(block);
    else
      ring_write(bridge.get()->from_network, block, settings.rx_channels);
  }
  void local_audio() {
    // Keep the ASIO memory ABI and WDM protocol intact. Continuous SRC bridges their
    // local callback clock to the PTP-paced network clock in both directions.
    std::vector<float> input(tx_block.size()), output(rx_block.size());
    const double interval = packet_duration_ms(settings.rate);
    double deadline = monotonic_ms() + interval;
    try {
      while (pump_running && !backend_failed) {
        for (int n = 0; n < 8 && read_local(input); ++n)
          tx_buffer->write(input);
        const auto now = monotonic_ms();
        if (now >= deadline) {
          auto count = static_cast<std::uint64_t>((now - deadline) / interval) + 1;
          if (count > 8) {
            deadline += (count - 8) * interval;
            count = 8;
          }
          for (std::uint64_t n = 0; n < count; ++n) {
            rx_buffer->read(output);
            if (wdm || InterlockedCompareExchange(&bridge.get()->asio_pid, 0, 0))
              write_local(output);
            deadline += interval;
          }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    } catch (...) {
      backend_failed = true;
    }
  }
  void meter(std::span<const float> data, int channels, std::array<std::atomic<float>, 32>& peaks) {
    std::array<float, 32> current{};
    for (std::size_t i = 0; i < data.size(); ++i)
      current[i % channels] = std::max(current[i % channels], std::abs(data[i]));
    for (int c = 0; c < channels; ++c)
      peaks[c].store(std::max(current[c], peaks[c].load() * .85f));
  }
  bool capture(std::span<Pcm> packets) {
    if (backend_failed)
      return false;
    if (tx_buffer) {
      tx_buffer->read(tx_block);
    } else if (wdm) {
      try {
        if (!wdm->capture(tx_block))
          return false;
      } catch (...) {
        backend_failed = true;
        return false;
      }
    } else {
      auto& ring = bridge.get()->to_network;
      if (ring_available(ring) < frames_per_packet)
        return false;
      ring_read(ring, tx_block, settings.tx_channels);
    }
    meter(tx_block, settings.tx_channels, tx_peaks);
    float_to_pcm(tx_block, packets, settings.tx_channels);
    InterlockedAdd64(&bridge.get()->network_tx_frames, frames_per_packet);
    return true;
  }
  void playback(std::span<const Pcm> packets) {
    if (backend_failed)
      return;
    pcm_to_float(packets, rx_block, settings.rx_channels);
    meter(rx_block, settings.rx_channels, rx_peaks);
    if (rx_buffer) {
      rx_buffer->write(rx_block);
    } else if (wdm) {
      try {
        wdm->playback(rx_block);
      } catch (...) {
        backend_failed = true;
        return;
      }
    } else
      ring_write(bridge.get()->from_network, rx_block, settings.rx_channels);
    InterlockedAdd64(&bridge.get()->network_rx_frames, frames_per_packet);
  }
  Json status() const {
    auto out = session->status();
    auto* d = bridge.get();
    out["backend"] = settings.backend;
    out["preferredBufferFrames"] = bridge.preferred_buffer();
    out["activeBufferFrames"] = bridge.active_buffer();
    out["asioHostPid"] = InterlockedCompareExchange(&d->asio_pid, 0, 0);
    out["asioCallbacks"] = load64(d->asio_callbacks);
    out["networkTxFrames"] = load64(d->network_tx_frames);
    out["networkRxFrames"] = load64(d->network_rx_frames);
    out["txDropped"] = load64(d->to_network.dropped);
    out["rxDropped"] = load64(d->from_network.dropped);
    out["rxUnderruns"] = load64(d->from_network.underruns);
    out["bufferUnderruns"] = load64(d->from_network.underruns);
    if (tx_buffer) {
      out["clockBufferMs"] = 20;
      out["clockTxDriftPpm"] = tx_buffer->drift_ppm();
      out["clockRxDriftPpm"] = rx_buffer->drift_ppm();
      out["clockTxQueuedFrames"] = tx_buffer->available();
      out["clockRxQueuedFrames"] = rx_buffer->available();
      out["clockDroppedFrames"] = tx_buffer->dropped() + rx_buffer->dropped();
      out["clockUnderrunFrames"] = tx_buffer->underruns() + rx_buffer->underruns();
      out["bufferUnderruns"] = out["bufferUnderruns"].get<std::uint64_t>() +
                               tx_buffer->underruns() + rx_buffer->underruns();
    }
    out["wdm"] = wdm ? wdm->status() : Json(nullptr);
    out["clockMode"] = "Software";
    out["synchronized"] = false;
    out["ptpAvailable"] = out.at("clock").value("ptpAvailable", false);
    out["txPeaks"] = Json::array();
    out["rxPeaks"] = Json::array();
    for (int c = 0; c < settings.tx_channels; ++c)
      out["txPeaks"].push_back(tx_peaks[c].load());
    for (int c = 0; c < settings.rx_channels; ++c)
      out["rxPeaks"].push_back(rx_peaks[c].load());
    if (backend_failed) {
      out["running"] = false;
      out["error"] =
          "Audio driver transport failed; stop the soundcard and check the installed driver";
    }
    return out;
  }
};
Engine::Engine(Settings s, std::shared_ptr<ptp::Monitor> clock)
    : impl_(std::make_unique<Impl>(std::move(s), std::move(clock))) {}
Engine::~Engine() = default;
Json Engine::status() const {
  return impl_->status();
}
} // namespace aoip::vsc
#endif
