#include "aoip/session.hpp"
#include "aoip/discovery.hpp"
#include "aoip/network.hpp"
#include "aoip/ptp.hpp"
#include <atomic>
#include <cmath>
#include <mutex>
#include <thread>
namespace aoip {
void pcm_to_float(std::span<const Pcm> packets, std::span<float> output, int channels) {
  if (channels < 1 || channels > 32 || packets.size() < std::size_t((channels + 1) / 2) ||
      output.size() != std::size_t(frames_per_packet * channels))
    throw std::runtime_error("PCM channel dimensions do not match");
  for (int f = 0; f < frames_per_packet; ++f)
    for (int c = 0; c < channels; ++c) {
      const auto& p = packets[c / 2];
      const auto at = f * 4 + c % 2 * 2;
      const int raw = (int(p[at]) << 8) | p[at + 1];
      output[f * channels + c] = (raw >= 32768 ? raw - 65536 : raw) / 32768.f;
    }
}
void float_to_pcm(std::span<const float> input, std::span<Pcm> packets, int channels) {
  if (channels < 1 || channels > 32 || packets.size() < std::size_t((channels + 1) / 2) ||
      input.size() != std::size_t(frames_per_packet * channels))
    throw std::runtime_error("Float channel dimensions do not match");
  for (auto& p : packets)
    p.fill(0);
  for (int f = 0; f < frames_per_packet; ++f)
    for (int c = 0; c < channels; ++c) {
      const float v = input[f * channels + c];
      const auto raw = static_cast<std::uint16_t>(static_cast<std::int16_t>(
          std::lround(std::clamp(std::isfinite(v) ? v : 0.f, -1.f, 1.f) * 32767)));
      auto& p = packets[c / 2];
      const auto at = f * 4 + c % 2 * 2;
      p[at] = std::uint8_t(raw >> 8);
      p[at + 1] = std::uint8_t(raw);
    }
}
struct AudioSession::Impl {
  SessionConfig config;
  std::shared_ptr<ptp::Monitor> ptp_monitor;
  std::atomic<bool> running{true};
  std::atomic<double> last_plan{0};
  mutable std::mutex mutex;
  std::unique_ptr<NetworkDevice> node;
  std::thread worker;
  std::string error, url, network_error;
  mutable double sampled_at = 0, tx_bps = 0, rx_bps = 0;
  mutable std::uint64_t last_tx = 0, last_rx = 0;
  explicit Impl(SessionConfig c) : config(std::move(c)) {
    if (!valid_id(config.id) || config.name.empty() || config.name.size() > 80 ||
        !valid_ipv4(config.interface_ip) || config.tx_channels < 1 || config.tx_channels > 32 ||
        config.rx_channels < 1 || config.rx_channels > 32 || !supported_sample_rate(config.rate))
      throw std::runtime_error("Invalid audio session identity, interface or channel count");
    if (config.latency_ms < 20 || config.latency_ms > 500)
      throw std::runtime_error("Network latency must be 20-500 ms");
    if (config.ptp_domain < 0 || config.ptp_domain > 255)
      throw std::runtime_error("PTP domain must be 0-255");
    if (config.clock_source != "internal" && config.clock_source != "ptp-monitor")
      throw std::runtime_error("Audio PTP clock discipline is not implemented; use Internal or the "
                               "experimental ptp-monitor mode");
    if (config.clock_source == "ptp-monitor")
      ptp_monitor = ptp::shared_monitor(config.interface_ip, config.ptp_domain);
    worker = std::thread([this] { run(); });
  }
  ~Impl() {
    running = false;
    if (worker.joinable())
      worker.join();
  }
  void run() {
    try {
      while (running) {
        if (!node) {
          auto controller = config.controller;
          if (controller == "auto") {
            const auto controllers = discover_controllers(config.interface_ip, 300);
            if (controllers.empty()) {
              for (int n = 0; n < 10 && running; ++n)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
              continue;
            }
            controller = controllers.front().url;
          }
          DeviceConfig c;
          c.id = config.id;
          c.name = config.name;
          c.controller = controller;
          c.host = config.interface_ip;
          c.tx_channels = config.tx_channels;
          c.rx_channels = config.rx_channels;
          c.tx_names = config.tx_names;
          c.rx_names = config.rx_names;
          c.clock_status = [this] { return clock_status(); };
          c.sender.streams = (config.tx_channels + 1) / 2;
          c.receiver.inputs = (config.rx_channels + 1) / 2;
          c.sender.rate = c.receiver.rate = config.rate;
          c.receiver.port = c.receiver.monitor_port = 0;
          c.receiver.latency_ms = config.latency_ms;
          c.sender.external_capture = config.capture;
          c.receiver.external_playback = config.playback;
          c.on_plan = [this](const Json&) {
            last_plan = monotonic_ms();
            std::lock_guard lock(mutex);
            network_error.clear();
          };
          c.on_error = [this](const std::string& e) {
            std::lock_guard lock(mutex);
            network_error = e;
          };
          auto device = std::make_unique<NetworkDevice>(c);
          std::lock_guard lock(mutex);
          url = controller;
          node = std::move(device);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
    } catch (const std::exception& e) {
      std::lock_guard lock(mutex);
      error = e.what();
    }
    std::unique_ptr<NetworkDevice> retired;
    {
      std::lock_guard lock(mutex);
      retired = std::move(node);
    }
    retired.reset();
  }
  Json clock_status() const {
    if (ptp_monitor)
      return ptp_monitor->status();
    return {{"source", "internal"},  {"mode", "Internal / software"}, {"locked", false},
            {"synchronized", false}, {"audioLocked", false},          {"ptpAvailable", false},
            {"ptpSupported", true},  {"trackingStable", false},       {"state", "Free running"}};
  }
  Json status() const {
    std::lock_guard lock(mutex);
    const auto now = monotonic_ms();
    const auto d = node ? node->describe() : Json(nullptr);
    std::uint64_t tx = 0, rx = 0, lost = 0, late = 0, send_errors = 0;
    if (!d.is_null()) {
      for (const auto& stream : d.at("streams")) {
        const auto& s = stream.at("stats");
        tx += s.value("bytes", 0ULL);
        send_errors += s.value("errors", 0ULL);
      }
      for (const auto& input : d.at("inputs")) {
        const auto& s = input.at("stats");
        rx += s.value("received", 0ULL) * 972;
        lost += s.value("lost", 0ULL);
        late += s.value("late", 0ULL);
      }
    }
    if (sampled_at == 0) {
      sampled_at = now;
      last_tx = tx;
      last_rx = rx;
    } else if (now - sampled_at >= 250) {
      const double seconds = (now - sampled_at) / 1000.;
      tx_bps = (tx >= last_tx ? tx - last_tx : 0) * 8 / seconds;
      rx_bps = (rx >= last_rx ? rx - last_rx : 0) * 8 / seconds;
      sampled_at = now;
      last_tx = tx;
      last_rx = rx;
    }
    return {{"running", running.load() && error.empty()},
            {"error", error},
            {"networkError", network_error},
            {"kind", config.kind},
            {"controller", url},
            {"controllerConnected", last_plan.load() > 0 && now - last_plan.load() < 2500},
            {"sampleRate", config.rate},
            {"txChannels", config.tx_channels},
            {"rxChannels", config.rx_channels},
            {"txBytes", tx},
            {"rxBytes", rx},
            {"txBitsPerSecond", tx_bps},
            {"rxBitsPerSecond", rx_bps},
            {"packetLoss", lost},
            {"latePackets", late},
            {"sendErrors", send_errors},
            {"clock", clock_status()},
            {"device", d}};
  }
};
AudioSession::AudioSession(SessionConfig c) : impl_(std::make_unique<Impl>(std::move(c))) {}
AudioSession::~AudioSession() = default;
Json AudioSession::status() const {
  return impl_->status();
}
} // namespace aoip
