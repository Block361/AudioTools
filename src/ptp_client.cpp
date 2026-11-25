// SPDX-License-Identifier: MIT
#include "aoip/ptp.hpp"
#include <atomic>
#include <chrono>
#include <mutex>
#include <numeric>
#include <thread>
namespace aoip::ptp {
namespace {
constexpr char multicast_address[] = "224.0.1.129";
PortIdentity local_identity() {
  PortIdentity id;
  for (int at = 0; at < 8; at += 4) {
    const auto random = random_uint32();
    for (int i = 0; i < 4; ++i)
      id.clock[at + i] = std::uint8_t(random >> (i * 8));
  }
  id.clock[0] = (id.clock[0] & 0xfc) | 2;
  return id;
}
struct SessionClock {
  Time monotonic, utc;
  SessionClock() {
    const auto a = monotonic_ns();
    utc = std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::system_clock::now().time_since_epoch())
              .count();
    monotonic = std::midpoint(a, monotonic_ns());
  }
  Time at(Time point) const {
    return utc + (point - monotonic);
  }
  Time now() const {
    return at(monotonic_ns());
  }
};
} // namespace
struct Monitor::Impl {
  Options options;
  SessionClock clock;
  Tracker tracker;
  std::unique_ptr<UdpSocket> event, general;
  mutable std::mutex mutex;
  std::atomic<bool> running{true};
  std::thread worker;
  std::string error;
  std::uint64_t malformed = 0, send_errors = 0;
  explicit Impl(Options o) : options(std::move(o)), tracker(local_identity(), options.domain) {
    if (!valid_ipv4(options.interface_ip) || options.interface_ip == "0.0.0.0")
      throw std::runtime_error(
          "PTPv2 monitor requires an explicit local IPv4 interface; select your LAN address");
    if (options.event_port < 0 || options.event_port > 65535 || options.general_port < 0 ||
        options.general_port > 65535 || options.destination_event_port < 1 ||
        options.destination_event_port > 65535 ||
        (options.event_port && options.event_port == options.general_port))
      throw std::runtime_error("Invalid PTP UDP ports");
    const auto bind = options.multicast ? "0.0.0.0" : options.interface_ip;
    try {
      event = std::make_unique<UdpSocket>(options.event_port, bind, true);
      general = std::make_unique<UdpSocket>(options.general_port, bind, true);
    } catch (const std::exception& e) {
      throw std::runtime_error(std::string("Cannot open PTP event/general ports (319/320 by "
                                           "default); another PTP service may own them: ") +
                               e.what());
    }
    if (options.multicast) {
      event->join_multicast(multicast_address, options.interface_ip);
      general->join_multicast(multicast_address, options.interface_ip);
    }
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
        for (const bool events : {true, false}) {
          auto& socket = events ? *event : *general;
          for (int n = 0; n < 128; ++n) {
            const auto data = socket.receive_from();
            if (!data)
              break;
            try {
              const auto message = decode(data->data);
              if (events != (message.type == Type::Sync || message.type == Type::DelayReq))
                throw std::runtime_error("PTP message on the wrong UDP port");
              std::lock_guard lock(mutex);
              tracker.receive(message, data->host, clock.at(data->received_ns));
            } catch (const std::exception&) {
              std::lock_guard lock(mutex);
              ++malformed;
            }
          }
        }
        {
          std::lock_guard lock(mutex);
          const auto now = clock.now();
          tracker.tick(now);
          if (const auto request = tracker.delay_request(now)) {
            const auto target =
                options.multicast ? std::string(multicast_address) : tracker.master_address();
            const auto data = encode(*request);
            if (!event->send(data, target, options.destination_event_port)) {
              tracker.send_failed(request->sequence);
              ++send_errors;
            }
          }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    } catch (const std::exception& e) {
      std::lock_guard lock(mutex);
      error = e.what();
      running = false;
    }
  }
  Json status() const {
    std::lock_guard lock(mutex);
    auto out = tracker.status(clock.now());
    out["interface"] = options.interface_ip;
    out["eventPort"] = event->port();
    out["generalPort"] = general->port();
    out["malformedPackets"] = malformed;
    out["sendErrors"] = send_errors;
    out["error"] = error;
    if (!error.empty()) {
      out["state"] = "PTP transport error";
      out["trackingStable"] = false;
      out["measurementFresh"] = false;
    }
    return out;
  }
};
Monitor::Monitor(Options o) : impl_(std::make_unique<Impl>(std::move(o))) {}
Monitor::~Monitor() = default;
Json Monitor::status() const {
  return impl_->status();
}
int Monitor::event_port() const {
  return impl_->event->port();
}
int Monitor::general_port() const {
  return impl_->general->port();
}
std::shared_ptr<Monitor> shared_monitor(const std::string& ip, int domain) {
  Options options;
  options.interface_ip = ip;
  options.domain = domain;
  return shared_monitor(std::move(options));
}
std::shared_ptr<Monitor> shared_monitor(Options options) {
  static std::mutex mutex;
  static std::weak_ptr<Monitor> active;
  static Options selected;
  std::lock_guard lock(mutex);
  if (auto monitor = active.lock()) {
    if (options.interface_ip != selected.interface_ip || options.domain != selected.domain ||
        options.event_port != selected.event_port ||
        options.general_port != selected.general_port ||
        options.destination_event_port != selected.destination_event_port ||
        options.multicast != selected.multicast)
      throw std::runtime_error("This experimental PTP engine supports one interface/domain at a "
                               "time; stop the other PTP session before changing it");
    return monitor;
  }
  auto monitor = std::make_shared<Monitor>(options);
  selected = options;
  active = monitor;
  return monitor;
}
} // namespace aoip::ptp
