#include "aoip/audio.hpp"
#include "aoip/network.hpp"
#include "aoip/system.hpp"
#include "device_common.hpp"
#include "httplib.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace aoip {
namespace detail {
using namespace std::chrono_literals;
void wait_running(const std::atomic<bool>& running, int ms) {
  for (int i = 0; running && i < ms; i += 10)
    std::this_thread::sleep_for(10ms);
}
void validate_common(const std::string& id, const std::string& name, const std::string& controller,
                     const std::string& host) {
  if (!valid_id(id) || name.empty() || name.size() > 80)
    throw std::runtime_error("Invalid device ID or name");
  if (!valid_ipv4(host))
    throw std::runtime_error("Bind host must be an IPv4 address");
  if (!controller.starts_with("http://"))
    throw std::runtime_error("Controller URL must start with http://");
}
void heartbeat(const std::atomic<bool>& running, const std::string& controller,
               const std::function<Json()>& payload, const std::function<void(const Json&)>& apply,
               const std::string& bind_host,
               const std::function<void(const std::string&)>& on_error) {
  std::string last_error;
  while (running) {
    try {
      httplib::Client client(controller);
      if (bind_host != "0.0.0.0")
        client.set_interface(bind_host);
      client.set_connection_timeout(1, 0);
      client.set_read_timeout(1, 0);
      client.set_write_timeout(1, 0);
      auto response = client.Post("/api/heartbeat", payload().dump(), "application/json");
      if (!response)
        throw std::runtime_error("Controller unavailable");
      const auto result = Json::parse(response->body);
      if (response->status != 200)
        throw std::runtime_error(result.value("error", "Heartbeat rejected"));
      if (running)
        apply(result);
      last_error.clear();
    } catch (const std::exception& e) {
      if (running && last_error != e.what())
        std::cerr << e.what() << "; retrying\n";
      last_error = e.what();
      if (on_error)
        on_error(last_error);
    }
    wait_running(running, 500);
  }
}
struct Destination {
  std::string host;
  int port;
};
Json audio_status(const AudioIoStatus& status, const std::string& mode) {
  return {{"mode", mode},
          {"device", status.device},
          {"backend", status.backend},
          {"state", status.state},
          {"channels", status.channels},
          {"channelMap", status.map},
          {"frames", status.frames},
          {"droppedFrames", status.dropped_frames},
          {"underrunFrames", status.underrun_frames},
          {"driftCorrections", status.drift_corrections}};
}
} // namespace detail
using namespace detail;
struct Sender::Impl {
  struct Stream {
    std::string id, name, source;
    std::uint32_t ssrc = random_uint32(), timestamp = random_uint32();
    std::uint16_t sequence = static_cast<std::uint16_t>(random_uint32());
    std::uint64_t frame_offset = 0, packets = 0, bytes = 0, errors = 0, skipped = 0;
    double frequency = 440;
    Bytes wav;
    Levels level;
    std::vector<Destination> destinations;
  };
  SenderConfig config;
  std::string instance = random_id();
  UdpSocket socket;
  std::vector<Stream> streams;
  std::unique_ptr<CaptureDevice> capture;
  std::vector<Pcm> capture_blocks;
  std::mutex mutex;
  std::atomic<bool> running{true};
  std::thread worker, heart;
  explicit Impl(SenderConfig c) : config(std::move(c)), socket(0, config.host) {
    packet_duration_ms(config.rate);
    validate_common(config.id, config.name, config.controller, config.host);
    if (config.streams < 1 || config.streams > 16 ||
        config.wav.size() > static_cast<std::size_t>(config.streams))
      throw std::runtime_error("Expected 1-16 streams and no more WAV files than streams");
    if (!std::isfinite(config.tone) || config.tone < 20 || config.tone > 18000)
      throw std::runtime_error("Tone frequency must be 20-18000 Hz");
    if (!config.capture_device.empty()) {
      if (!config.wav.empty())
        throw std::runtime_error("Choose either --capture or --wav");
      capture = std::make_unique<CaptureDevice>(config.capture_device, config.streams,
                                                config.capture_channels, config.audio_backend,
                                                config.rate);
      capture_blocks.resize(static_cast<std::size_t>(config.streams));
    } else if (!config.capture_channels.empty())
      throw std::runtime_error("--capture-map needs --capture");
    for (int i = 0; i < config.streams; ++i) {
      Stream s;
      s.id = "stream-" + std::to_string(i + 1);
      s.frequency = std::min(config.tone + i * 110, 18000.0);
      if (capture) {
        const auto hardware = capture->status();
        s.name = "Input " + std::to_string(hardware.map[i * 2]) + "/" +
                 std::to_string(hardware.map[i * 2 + 1]);
        s.source = "Hardware: " + hardware.device;
      } else if (static_cast<std::size_t>(i) < config.wav.size()) {
        s.wav = load_wav(config.wav[i], config.rate);
        s.name = std::filesystem::path(config.wav[i]).filename().string();
        s.source = "WAV: " + s.name;
      } else if (config.external_capture) {
        s.name = "Virtual " + std::to_string(i * 2 + 1) + "/" + std::to_string(i * 2 + 2);
        s.source = "Virtual audio bridge";
      } else {
        s.name = "Tone " + std::to_string(static_cast<int>(s.frequency)) + " Hz";
        s.source = "Test tone";
      }
      streams.push_back(std::move(s));
    }
    if (config.external_capture)
      capture_blocks.resize(config.streams);
  }
  void start() {
    if (capture)
      capture->start();
    worker = std::thread([this] { run(); });
    try {
      if (config.register_with_controller)
        heart = std::thread([this] {
          heartbeat(
              running, config.controller, [this] { return describe(); },
              [this](const Json& plan) { apply(plan); }, config.host);
        });
    } catch (...) {
      running = false;
      worker.join();
      throw;
    }
  }
  ~Impl() {
    running = false;
    if (heart.joinable())
      heart.join();
    if (worker.joinable())
      worker.join();
  }
  Json describe() {
    std::lock_guard lock(mutex);
    Json outputs = Json::array();
    for (const auto& s : streams)
      outputs.push_back({{"id", s.id},
                         {"name", s.name},
                         {"ssrc", s.ssrc},
                         {"source", s.source},
                         {"stats",
                          {{"packets", s.packets},
                           {"bytes", s.bytes},
                           {"errors", s.errors},
                           {"schedulerSkipped", s.skipped},
                           {"rms", s.level.rms},
                           {"peak", s.level.peak},
                           {"destinations", s.destinations.size()}}}});
    Json result = {{"id", config.id},
                   {"name", config.name},
                   {"role", "sender"},
                   {"instance", instance},
                   {"streams", outputs}};
    result["audio"] =
        capture ? audio_status(capture->status(), "capture") : Json{{"mode", "generated"}};
    if (config.external_capture)
      result["audio"] = {{"mode", "virtual"}, {"backend", "Shared memory"}};
    result["format"] = {{"sampleRate", config.rate}, {"bits", 16}, {"channels", 2}};
    return result;
  }
  void apply(const Json& plan) {
    std::lock_guard lock(mutex);
    for (auto& s : streams) {
      s.destinations.clear();
      for (const auto& d : plan.at("sends"))
        if (d.at("stream") == s.id) {
          const auto host = d.at("host").get<std::string>();
          const int port = d.at("port").get<int>();
          if (valid_ipv4(host) && port >= 1024 && port <= 65535)
            if (std::none_of(s.destinations.begin(), s.destinations.end(), [&](const auto& item) {
                  return item.host == host && item.port == port;
                }))
              s.destinations.push_back({host, port});
        }
    }
  }
  void tick(std::span<const Pcm> captured = {}) {
    for (std::size_t index = 0; index < streams.size(); ++index) {
      auto& s = streams[index];
      if (!captured.empty())
        s.level = levels(captured[index]);
      if (!s.destinations.empty()) {
        Packet packet;
        packet.sequence = s.sequence;
        packet.timestamp = s.timestamp;
        packet.ssrc = s.ssrc;
        packet.pcm = !captured.empty() ? captured[index]
                     : s.wav.empty()   ? tone_packet(s.frame_offset, s.frequency, config.rate)
                                       : wav_packet(s.wav, s.frame_offset);
        s.level = levels(packet.pcm);
        const auto encoded = encode_packet(packet);
        for (const auto& destination : s.destinations) {
          if (!socket.send(encoded, destination.host, destination.port))
            ++s.errors;
          else {
            ++s.packets;
            s.bytes += encoded.size();
          }
        }
      } else if (captured.empty())
        s.level = {};
      ++s.sequence;
      s.timestamp += frames_per_packet;
      s.frame_offset += frames_per_packet;
    }
  }
  void run() {
    double deadline = monotonic_ms() + packet_duration_ms(config.rate);
    try {
      while (running) {
        const double interval =
            config.packet_interval ? config.packet_interval() : packet_duration_ms(config.rate);
        if (config.external_capture && config.packet_interval) {
          const double now = monotonic_ms();
          if (now >= deadline) {
            // Bound catch-up; a stalled OS must not release an unbounded burst of old audio.
            auto count = static_cast<std::uint64_t>((now - deadline) / interval) + 1;
            if (count > 8) {
              const auto skipped = count - 8;
              std::lock_guard lock(mutex);
              for (auto& s : streams) {
                s.skipped += skipped;
                s.sequence = std::uint16_t(s.sequence + skipped);
                s.timestamp += static_cast<std::uint32_t>(skipped * frames_per_packet);
                s.frame_offset += skipped * frames_per_packet;
              }
              deadline += skipped * interval;
              count = 8;
            }
            for (std::uint64_t n = 0; n < count && running; ++n) {
              if (!config.external_capture(capture_blocks))
                for (auto& block : capture_blocks)
                  block.fill(0);
              {
                std::lock_guard lock(mutex);
                tick(capture_blocks);
              }
              deadline += interval;
            }
          }
          std::this_thread::sleep_for(1ms);
          continue;
        }
        if (capture || config.external_capture) {
          // Capture hardware supplies the sample clock; transmit complete 240-frame blocks.
          while (running && (capture ? capture->read(capture_blocks)
                                     : config.external_capture(capture_blocks))) {
            std::lock_guard lock(mutex);
            tick(capture_blocks);
          }
          std::this_thread::sleep_for(1ms);
          continue;
        }
        const double now = monotonic_ms();
        if (now >= deadline) {
          auto count = static_cast<std::uint64_t>((now - deadline) / interval) + 1;
          std::lock_guard lock(mutex);
          if (count > 8) {
            const auto skipped = count - 8;
            deadline += skipped * interval;
            count = 8;
            for (auto& s : streams) {
              s.skipped += skipped;
              s.sequence = std::uint16_t(s.sequence + skipped);
              s.timestamp += static_cast<std::uint32_t>(skipped * frames_per_packet);
              s.frame_offset += skipped * frames_per_packet;
            }
          }
          for (std::uint64_t i = 0; i < count; ++i) {
            tick();
            deadline += interval;
          }
        }
        std::this_thread::sleep_for(1ms);
      }
    } catch (const std::exception& e) {
      std::cerr << "Sender stopped: " << e.what() << '\n';
      running = false;
    }
  }
};
Sender::Sender(SenderConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {
  impl_->start();
}
Sender::~Sender() = default;
Json Sender::describe() const {
  return impl_->describe();
}
void Sender::apply_plan(const Json& plan) {
  impl_->apply(plan);
}

struct NetworkDevice::Impl {
  DeviceConfig config;
  std::string instance = random_id();
  std::unique_ptr<Sender> sender;
  std::unique_ptr<Receiver> receiver;
  std::atomic<bool> running{true};
  std::thread heart;
  explicit Impl(DeviceConfig c) : config(std::move(c)) {
    if (config.sender.rate != config.receiver.rate)
      throw std::runtime_error("Send and receive sample rates must match");
    validate_common(config.id, config.name, config.controller, config.host);
    if (config.sender.streams < 0 || config.sender.streams > 16 || config.receiver.inputs < 0 ||
        config.receiver.inputs > 16 || (!config.sender.streams && !config.receiver.inputs))
      throw std::runtime_error("Device needs 0-16 send streams and 0-16 receive inputs, with at "
                               "least one direction enabled");
    if (config.sender.streams) {
      auto s = config.sender;
      s.id = config.id;
      s.name = config.name;
      s.controller = config.controller;
      s.host = config.host;
      s.register_with_controller = false;
      sender = std::make_unique<Sender>(s);
    }
    if (config.receiver.inputs) {
      auto r = config.receiver;
      r.id = config.id;
      r.name = config.name;
      r.controller = config.controller;
      r.host = config.host;
      r.register_with_controller = false;
      receiver = std::make_unique<Receiver>(r);
    }
  }
  void start() {
    if (!config.register_with_controller)
      return;
    heart = std::thread([this] {
      heartbeat(
          running, config.controller, [this] { return describe(); },
          [this](const Json& plan) {
            if (sender)
              sender->apply_plan(plan);
            if (receiver)
              receiver->apply_plan(plan);
            if (config.on_plan)
              config.on_plan(plan);
          },
          config.host, config.on_error);
    });
  }
  ~Impl() {
    running = false;
    if (heart.joinable())
      heart.join();
    // Best-effort release of this instance only. A crashed process is detected by heartbeat
    // timeout.
    if (!config.register_with_controller)
      return;
    try {
      httplib::Client client(config.controller);
      if (config.host != "0.0.0.0")
        client.set_interface(config.host);
      client.set_connection_timeout(0, 300000);
      client.set_read_timeout(0, 500000);
      client.set_write_timeout(0, 500000);
      client.Post("/api/goodbye", Json{{"id", config.id}, {"instance", instance}}.dump(),
                  "application/json");
    } catch (...) {
    }
  }
  Json describe() const {
    const auto tx = sender ? sender->describe() : Json::object();
    const auto rx = receiver ? receiver->describe() : Json::object();
    Json result = {{"id", config.id},
                   {"name", config.name},
                   {"role", "device"},
                   {"instance", instance},
                   {"streams", sender ? tx.at("streams") : Json::array()},
                   {"inputs", receiver ? rx.at("inputs") : Json::array()},
                   {"audio",
                    {{"mode", "device"},
                     {"capture", sender ? tx.at("audio") : Json{{"mode", "off"}}},
                     {"playback", receiver ? rx.at("audio") : Json{{"mode", "off"}}}}}};
    if (receiver)
      result["monitorPort"] = rx.at("monitorPort");
    result["format"] = {{"sampleRate", config.sender.rate}, {"bits", 16}, {"channels", 2}};
    result["clock"] = {{"mode", "software"}, {"synchronized", false}, {"ptpSupported", false}};
    if (config.clock_status)
      result["clock"] = config.clock_status();
    for (std::size_t i = 0; i < result["streams"].size(); ++i)
      result["streams"][i]["channels"] =
          config.tx_channels
              ? std::min(2, std::max(0, config.tx_channels - static_cast<int>(i) * 2))
              : 2;
    for (std::size_t i = 0; i < result["inputs"].size(); ++i)
      result["inputs"][i]["channels"] =
          config.rx_channels
              ? std::min(2, std::max(0, config.rx_channels - static_cast<int>(i) * 2))
              : 2;
    for (const auto* direction : {"streams", "inputs"}) {
      const auto& names = std::string(direction) == "streams" ? config.tx_names : config.rx_names;
      for (std::size_t i = 0; i < result[direction].size(); ++i) {
        auto& endpoint = result[direction][i];
        endpoint["labels"] = Json::array();
        for (int side = 0; side < endpoint.at("channels").get<int>(); ++side) {
          const auto at = i * 2 + side;
          endpoint["labels"].push_back(at < names.size() && !names[at].empty()
                                           ? names[at]
                                           : ("Channel " + std::to_string(at + 1)));
        }
      }
    }
    return result;
  }
};
NetworkDevice::NetworkDevice(DeviceConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {
  impl_->start();
}
NetworkDevice::~NetworkDevice() = default;
Json NetworkDevice::describe() const {
  return impl_->describe();
}
void NetworkDevice::apply_plan(const Json& plan) {
  if (impl_->sender)
    impl_->sender->apply_plan(plan);
  if (impl_->receiver)
    impl_->receiver->apply_plan(plan);
}
} // namespace aoip
