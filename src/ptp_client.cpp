// SPDX-License-Identifier: MIT
#include "aoip/audio_clock.hpp"
#include "aoip/ptp.hpp"
#include <atomic>
#include <chrono>
#include <map>
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
  PortIdentity self = local_identity();
  Tracker tracker;
  AudioRateServo rate_servo;
  Time next_servo = 0;
  std::unique_ptr<UdpSocket> event, general;
  mutable std::mutex mutex;
  std::atomic<bool> running{true};
  std::thread worker;
  std::string error;
  std::uint64_t malformed = 0, send_errors = 0;
  std::uint64_t announces = 0, syncs = 0, followups = 0, responses = 0, requests = 0, received = 0;
  Time next_announce = 0, next_sync = 0, last_announce = 0, last_sync = 0;
  std::uint16_t announce_sequence = 0, sync_sequence = 0;
  std::map<PortIdentity, Time> other_masters;
  explicit Impl(Options o) : options(std::move(o)), tracker(self, options.domain) {
    if (!valid_ipv4(options.interface_ip) || options.interface_ip == "0.0.0.0")
      throw std::runtime_error(
          "PTPv2 requires an explicit local IPv4 interface; select your LAN address");
    if (options.role != Role::Monitor && options.role != Role::Grandmaster)
      throw std::runtime_error("Unknown PTP role");
    if (options.event_port < 0 || options.event_port > 65535 || options.general_port < 0 ||
        options.general_port > 65535 || options.destination_event_port < 1 ||
        options.destination_event_port > 65535 || options.destination_general_port < 1 ||
        options.destination_general_port > 65535 ||
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
  bool send(UdpSocket& socket, const Message& message, const std::string& host, int port) {
    if (socket.send(encode(message), host, port))
      return true;
    ++send_errors;
    return false;
  }
  void serve(const Message& request, const std::string& host, Time arrival) {
    if (request.domain != options.domain || request.source == self)
      return;
    ++received;
    if (request.type == Type::Announce && request.clock_class != 255 &&
        request.grandmaster != self.clock) {
      if (other_masters.size() < 32 || other_masters.contains(request.source))
        other_masters[request.source] = arrival;
      return;
    }
    if (request.type != Type::DelayReq || request.correction_ns > 1000000000LL ||
        request.correction_ns < -1000000000LL)
      return;
    ++requests;
    Message response;
    response.type = Type::DelayResp;
    response.source = self;
    response.domain = std::uint8_t(options.domain);
    response.sequence = request.sequence;
    response.requesting = request.source;
    response.timestamp_ns = arrival;
    response.correction_ns = request.correction_ns;
    response.interval = 0;
    const bool unicast = !options.multicast || (request.flags & 0x0400);
    if (unicast)
      response.flags |= 0x0400;
    if (send(*general, response, unicast ? host : multicast_address,
             options.destination_general_port))
      ++responses;
  }
  void publish(Time now) {
    constexpr Time second = 1000000000;
    std::erase_if(other_masters,
                  [&](const auto& entry) { return now - entry.second > 6 * second; });
    const std::string target = options.multicast ? multicast_address : options.interface_ip;
    if (now >= next_announce) {
      Message a;
      a.type = Type::Announce;
      a.source = self;
      a.grandmaster = self.clock;
      a.domain = std::uint8_t(options.domain);
      a.sequence = ++announce_sequence;
      a.interval = 0;
      // Free-running software oscillator: class 248, unknown accuracy, arbitrary timescale.
      // No GPS/UTC/TAI traceability is advertised.
      if (send(*general, a, target, options.destination_general_port)) {
        ++announces;
        last_announce = now;
      }
      next_announce = now + second;
    }
    if (now >= next_sync) {
      Message s;
      s.type = Type::Sync;
      s.source = self;
      s.domain = std::uint8_t(options.domain);
      s.sequence = ++sync_sequence;
      s.flags = 0x0200;
      s.interval = -3;
      const auto bytes = encode(s);
      const Time origin = clock.now();
      if (event->send(bytes, target, options.destination_event_port)) {
        ++syncs;
        Message f = s;
        f.type = Type::FollowUp;
        f.flags = 0;
        f.timestamp_ns = origin;
        if (send(*general, f, target, options.destination_general_port)) {
          ++followups;
          last_sync = origin;
        }
      } else
        ++send_errors;
      next_sync = now + second / 8;
    }
  }
  Json grandmaster_status(Time now) const {
    const bool transmitting = last_announce && last_sync && now - last_announce < 3000000000LL &&
                              now - last_sync < 1000000000LL;
    const auto own_id = identity(self);
    return {
        {"source", "ptp-grandmaster"},
        {"role", "grandmaster"},
        {"mode", "PTPv2 software GM"},
        {"state", transmitting ? (other_masters.empty() ? "Grandmaster (software)"
                                                        : "Grandmaster / other GM detected")
                  : announces  ? "Grandmaster send failure"
                               : "Starting grandmaster"},
        {"clockReference", "Local OS/QPC software oscillator"},
        {"locked", false},
        {"synchronized", false},
        {"audioLocked", false},
        {"ptpAvailable", true},
        {"ptpSupported", true},
        {"experimental", true},
        {"trackingStable", false},
        {"measurementFresh", false},
        {"transmitting", transmitting},
        {"domain", options.domain},
        {"timestamping", "Application software"},
        {"localPortIdentity", own_id},
        {"masterPortIdentity", own_id},
        {"grandmasterIdentity", own_id.substr(0, 16)},
        {"masterAddress", options.interface_ip},
        {"ptpTimescale", false},
        {"utcOffsetValid", false},
        {"clockClass", 248},
        {"clockAccuracy", 254},
        {"timeSource", 160},
        {"offsetNs", nullptr},
        {"pathDelayNs", nullptr},
        {"frequencyPpm", nullptr},
        {"packets", received},
        {"announcesSent", announces},
        {"syncsSent", syncs},
        {"followUpsSent", followups},
        {"delayRequestsReceived", requests},
        {"delayResponsesSent", responses},
        {"otherGrandmasters", other_masters.size()},
        {"warning",
         other_masters.empty()
             ? ""
             : "Another grandmaster is announcing in this domain; use one explicit software GM"}};
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
              if (options.role == Role::Grandmaster)
                serve(message, data->host, clock.at(data->received_ns));
              else
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
          if (options.role == Role::Grandmaster)
            publish(now);
          else {
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
        }
        {
          std::lock_guard lock(mutex);
          const auto now = clock.now();
          if (now >= next_servo) {
            auto measurement =
                options.role == Role::Grandmaster ? grandmaster_status(now) : tracker.status(now);
            measurement["error"] = error;
            rate_servo.update(measurement, double(monotonic_ns()) / 1e9);
            next_servo = now + 100000000;
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
    auto out = options.role == Role::Grandmaster ? grandmaster_status(clock.now())
                                                 : tracker.status(clock.now());
    if (options.role == Role::Monitor)
      out["role"] = "monitor";
    out.update(rate_servo.status());
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
      out["transmitting"] = false;
      out["rateReferenceReady"] = false;
      out["rateState"] = "Holdover";
    }
    return out;
  }
};
Monitor::Monitor(Options o) : impl_(std::make_unique<Impl>(std::move(o))) {}
Monitor::~Monitor() = default;
Json Monitor::status() const {
  return impl_->status();
}
double Monitor::rate_ppm() const {
  return impl_->rate_servo.correction_ppm();
}
int Monitor::event_port() const {
  return impl_->event->port();
}
int Monitor::general_port() const {
  return impl_->general->port();
}
std::shared_ptr<Monitor> shared_monitor(const std::string& ip, int domain, Role role) {
  Options options;
  options.interface_ip = ip;
  options.domain = domain;
  options.role = role;
  return shared_monitor(std::move(options));
}
std::shared_ptr<Monitor> shared_monitor(Options options) {
  static std::mutex mutex;
  static std::weak_ptr<Monitor> active;
  static Options selected;
  std::lock_guard lock(mutex);
  if (auto monitor = active.lock()) {
    if (options.interface_ip != selected.interface_ip || options.domain != selected.domain ||
        options.role != selected.role || options.event_port != selected.event_port ||
        options.general_port != selected.general_port ||
        options.destination_event_port != selected.destination_event_port ||
        options.multicast != selected.multicast ||
        options.destination_general_port != selected.destination_general_port)
      throw std::runtime_error("PTP interface, domain and role must match between applications in "
                               "this engine; stop the other PTP session before changing them");
    return monitor;
  }
  auto monitor = std::make_shared<Monitor>(options);
  selected = options;
  active = monitor;
  return monitor;
}
} // namespace aoip::ptp
