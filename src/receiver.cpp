#include "aoip/network.hpp"
#include "aoip/system.hpp"
#include "device_common.hpp"
#include "httplib.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <thread>

namespace aoip {
using namespace std::chrono_literals;
struct Receiver::Impl {
  struct Lane {
    Json route = nullptr;
    JitterBuffer jitter;
    double last_packet = 0;
    explicit Lane(int latency, int rate) : jitter(latency, rate) {}
  };
  struct Channel {
    std::string id, name;
    UdpSocket socket;
    std::array<Lane, 2> lanes;
    AudioRing ring;
    std::uint64_t malformed = 0, foreign = 0, played = 0, skipped = 0, received = 0;
    Levels level;
    Channel(int i, int port, const ReceiverConfig& c)
        : id("input-" + std::to_string(i + 1)), name("Stereo input " + std::to_string(i + 1)),
          socket(port, c.host), lanes{Lane(c.latency_ms, c.rate), Lane(c.latency_ms, c.rate)},
          ring(c.rate) {}
  };
  ReceiverConfig config;
  std::string instance = random_id();
  std::vector<std::unique_ptr<Channel>> channels;
  std::unique_ptr<PlaybackDevice> playback;
  std::vector<Pcm> playback_blocks;
  std::mutex mutex;
  std::atomic<bool> running{true};
  std::thread worker, heart, monitor_thread;
  httplib::Server monitor;
  int monitor_port = 0;
  explicit Impl(ReceiverConfig c) : config(std::move(c)) {
    detail::validate_common(config.id, config.name, config.controller, config.host);
    packet_duration_ms(config.rate);
    if (config.inputs < 1 || config.inputs > 16 || config.port < 0 ||
        (config.port && (config.port < 1024 || config.port + config.inputs - 1 > 65535)))
      throw std::runtime_error("Invalid receiver input count or port range");
    if (config.monitor_port < 0 || config.monitor_port > 65535 ||
        (config.monitor_port && config.monitor_port < 1024))
      throw std::runtime_error("Invalid monitor port");
    for (int i = 0; i < config.inputs; ++i)
      channels.push_back(std::make_unique<Channel>(i, config.port ? config.port + i : 0, config));
    if (!config.playback_device.empty())
      playback = std::make_unique<PlaybackDevice>(config.playback_device, config.inputs,
                                                  config.playback_channels, config.volume,
                                                  config.audio_backend, config.rate);
    else if (!config.playback_channels.empty())
      throw std::runtime_error("--playback-map needs --playback");
    playback_blocks.resize(config.inputs);
    monitor.set_read_timeout(2, 0);
    monitor.set_write_timeout(2, 0);
    monitor.Get(R"(/audio/([a-zA-Z0-9_-]+)\.wav)", [this](const auto& req, auto& res) {
      Bytes snapshot;
      {
        std::lock_guard lock(mutex);
        for (const auto& c : channels)
          if (c->id == req.matches[1])
            snapshot = c->ring.snapshot();
      }
      if (snapshot.empty()) {
        res.status = 409;
        res.set_content("No captured audio", "text/plain");
        return;
      }
      const auto wav = encode_wav(snapshot, config.rate);
      res.set_header("Cache-Control", "no-store");
      res.set_content(reinterpret_cast<const char*>(wav.data()), wav.size(), "audio/wav");
    });
    monitor_port = config.monitor_port;
    if (!monitor_port)
      monitor_port = monitor.bind_to_any_port(config.host);
    else if (!monitor.bind_to_port(config.host, monitor_port))
      monitor_port = -1;
    if (monitor_port <= 0)
      throw std::runtime_error("Cannot bind receiver monitor port");
  }
  ~Impl() {
    stop();
  }
  void start() {
    try {
      if (playback)
        playback->start();
      monitor_thread = std::thread([this] { monitor.listen_after_bind(); });
      worker = std::thread([this] { run(); });
      if (config.register_with_controller)
        heart = std::thread([this] {
          detail::heartbeat(
              running, config.controller, [this] { return describe(); },
              [this](const Json& p) { apply(p); }, config.host);
        });
    } catch (...) {
      stop();
      throw;
    }
  }
  void stop() {
    running = false;
    if (heart.joinable())
      heart.join();
    if (worker.joinable())
      worker.join();
    monitor.stop();
    if (monitor_thread.joinable())
      monitor_thread.join();
  }
  std::string lane_state(const Lane& lane, double time) const {
    return lane.route.is_null()                                 ? "unrouted"
           : !lane.last_packet || time - lane.last_packet > 500 ? "waiting"
           : lane.jitter.playing()                              ? "receiving"
                                                                : "buffering";
  }
  Json describe() {
    std::lock_guard lock(mutex);
    auto inputs = Json::array();
    const auto time = monotonic_ms();
    for (const auto& ptr : channels) {
      const auto& c = *ptr;
      const auto& a = c.lanes[0];
      const auto& b = c.lanes[1];
      const auto sa = lane_state(a, time), sb = lane_state(b, time);
      const auto state = sa == "receiving" || sb == "receiving"   ? "receiving"
                         : sa == "buffering" || sb == "buffering" ? "buffering"
                         : sa == "waiting" || sb == "waiting"     ? "waiting"
                                                                  : "unrouted";
      std::string source;
      if (!a.route.is_null())
        source = a.route.at("label").get<std::string>();
      if (!b.route.is_null() && (a.route.is_null() || a.route.at("label") != b.route.at("label")))
        source += (source.empty() ? "" : " | ") + b.route.at("label").get<std::string>();
      Json lane_stats = Json::array();
      for (int side = 0; side < 2; ++side) {
        const auto& l = c.lanes[side];
        lane_stats.push_back({{"channel", side + 1},
                              {"state", lane_state(l, time)},
                              {"received", l.jitter.stats.received},
                              {"lost", l.jitter.stats.lost},
                              {"late", l.jitter.stats.late},
                              {"route", l.route}});
      }
      const auto& x = a.jitter.stats;
      const auto& y = b.jitter.stats;
      inputs.push_back({{"id", c.id},
                        {"name", c.name},
                        {"port", c.socket.port()},
                        {"stats",
                         {{"received", c.received},
                          {"lost", std::max(x.lost, y.lost)},
                          {"late", std::max(x.late, y.late)},
                          {"duplicates", std::max(x.duplicates, y.duplicates)},
                          {"reordered", std::max(x.reordered, y.reordered)},
                          {"resets", std::max(x.resets, y.resets)},
                          {"jitterMs", std::max(x.jitter_ms, y.jitter_ms)},
                          {"malformed", c.malformed},
                          {"foreign", c.foreign},
                          {"played", c.played},
                          {"schedulerSkipped", c.skipped},
                          {"rms", c.level.rms},
                          {"peak", c.level.peak},
                          {"bufferedPackets", std::max(a.jitter.queued(), b.jitter.queued())},
                          {"latencyMs", config.latency_ms},
                          {"state", state},
                          {"source", source.empty() ? Json(nullptr) : Json(source)},
                          {"capturedSeconds", c.ring.seconds()},
                          {"channelStats", lane_stats}}}});
    }
    Json result = {{"id", config.id},
                   {"name", config.name},
                   {"role", "receiver"},
                   {"instance", instance},
                   {"monitorPort", monitor_port},
                   {"inputs", inputs},
                   {"format", {{"sampleRate", config.rate}, {"bits", 16}, {"channels", 2}}}};
    result["audio"] =
        playback ? detail::audio_status(playback->status(), "playback") : Json{{"mode", "monitor"}};
    if (playback)
      result["audio"]["volumePercent"] = std::lround(config.volume * 100);
    if (config.external_playback)
      result["audio"] = {{"mode", "virtual"}, {"backend", "Shared memory"}};
    return result;
  }
  void apply(const Json& plan) {
    std::lock_guard lock(mutex);
    for (auto& c : channels) {
      bool changed = false;
      for (int side = 0; side < 2; ++side) {
        Json route = nullptr;
        for (const auto& r : plan.at("receives"))
          if (r.at("input") == c->id &&
              (!r.contains("receiverChannel") || r.at("receiverChannel") == side + 1)) {
            route = r;
            route["senderChannel"] = r.value("senderChannel", side + 1);
            route["receiverChannel"] = side + 1;
          }
        auto& l = c->lanes[side];
        if (route != l.route) {
          l.route = route;
          l.jitter = JitterBuffer(config.latency_ms, config.rate);
          l.last_packet = 0;
          changed = true;
        }
      }
      if (changed) {
        c->ring.reset();
        c->received = 0;
        c->played = 0;
        c->level = {};
      }
    }
  }
  void run() {
    const double interval = packet_duration_ms(config.rate);
    double deadline = monotonic_ms() + interval;
    try {
      while (running) {
        {
          std::lock_guard lock(mutex);
          for (auto& c : channels)
            for (int i = 0; i < 128; ++i) {
              const auto data = c->socket.receive();
              if (!data)
                break;
              if (c->lanes[0].route.is_null() && c->lanes[1].route.is_null())
                continue;
              try {
                const auto p = decode_packet(*data);
                bool accepted = false;
                for (auto& l : c->lanes)
                  if (!l.route.is_null() && p.ssrc == l.route.at("ssrc").get<std::uint32_t>()) {
                    const auto t = monotonic_ms();
                    l.jitter.push(p, t);
                    l.last_packet = t;
                    accepted = true;
                  }
                if (accepted)
                  ++c->received;
                else
                  ++c->foreign;
              } catch (...) {
                ++c->malformed;
              }
            }
          const auto time = monotonic_ms();
          if (time >= deadline) {
            auto count = static_cast<std::uint64_t>((time - deadline) / interval) + 1;
            if (count > 8) {
              const auto skipped = count - 8;
              deadline += skipped * interval;
              count = 8;
              for (auto& c : channels) {
                c->skipped += skipped;
                for (auto& l : c->lanes) {
                  l.jitter.reset();
                  ++l.jitter.stats.resets;
                }
              }
            }
            for (std::uint64_t tick = 0; tick < count; ++tick) {
              for (std::size_t index = 0; index < channels.size(); ++index) {
                auto& c = *channels[index];
                Pcm output{};
                bool played = false;
                for (int side = 0; side < 2; ++side) {
                  auto& l = c.lanes[side];
                  const auto pcm = l.jitter.take(deadline);
                  if (!pcm || l.route.is_null())
                    continue;
                  played = true;
                  const int source = l.route.at("senderChannel").get<int>() - 1;
                  for (int f = 0; f < frames_per_packet; ++f) {
                    output[f * 4 + side * 2] = (*pcm)[f * 4 + source * 2];
                    output[f * 4 + side * 2 + 1] = (*pcm)[f * 4 + source * 2 + 1];
                  }
                }
                playback_blocks[index] = output;
                if (played) {
                  c.ring.push(output);
                  c.level = levels(output);
                  ++c.played;
                } else
                  c.level = {};
              }
              if (playback)
                playback->write(playback_blocks);
              if (config.external_playback)
                config.external_playback(playback_blocks);
              deadline += interval;
            }
          }
        }
        std::this_thread::sleep_for(1ms);
      }
    } catch (const std::exception& e) {
      std::cerr << "Receiver stopped: " << e.what() << '\n';
      running = false;
    }
  }
};
Receiver::Receiver(ReceiverConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {
  impl_->start();
}
Receiver::~Receiver() = default;
Json Receiver::describe() const {
  return impl_->describe();
}
void Receiver::apply_plan(const Json& plan) {
  impl_->apply(plan);
}
} // namespace aoip
