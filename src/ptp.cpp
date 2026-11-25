// SPDX-License-Identifier: MIT
#include "aoip/ptp.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <deque>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <sstream>
#include <tuple>
namespace aoip::ptp {
namespace {
constexpr Time second = 1000000000;
std::uint64_t get(std::span<const std::uint8_t> p, std::size_t at, int count) {
  std::uint64_t v = 0;
  for (int i = 0; i < count; ++i)
    v = (v << 8) | p[at + i];
  return v;
}
void put(Bytes& p, std::size_t at, std::uint64_t value, int count) {
  for (int i = count - 1; i >= 0; --i) {
    p[at + i] = std::uint8_t(value);
    value >>= 8;
  }
}
Time add(Time a, Time b) {
  if ((b > 0 && a > std::numeric_limits<Time>::max() - b) ||
      (b < 0 && a < std::numeric_limits<Time>::min() - b))
    throw std::runtime_error("PTP timestamp arithmetic overflow");
  return a + b;
}
Time sub(Time a, Time b) {
  if (b == std::numeric_limits<Time>::min())
    throw std::runtime_error("PTP timestamp arithmetic overflow");
  return add(a, -b);
}
Time stamp(std::span<const std::uint8_t> p, std::size_t at) {
  const auto s = get(p, at, 6), ns = get(p, at + 6, 4);
  if (ns >= second || s > (std::uint64_t(std::numeric_limits<Time>::max()) - ns) / second)
    throw std::runtime_error("Invalid PTP timestamp");
  return Time(s * second + ns);
}
void stamp(Bytes& p, std::size_t at, Time value) {
  if (value < 0)
    throw std::runtime_error("Negative PTP timestamp");
  put(p, at, value / second, 6);
  put(p, at + 6, value % second, 4);
}
PortIdentity port(std::span<const std::uint8_t> p, std::size_t at) {
  PortIdentity result;
  std::copy_n(p.begin() + at, 8, result.clock.begin());
  result.port = std::uint16_t(get(p, at + 8, 2));
  if (!result.port)
    throw std::runtime_error("Invalid PTP port identity");
  return result;
}
void port(Bytes& p, std::size_t at, const PortIdentity& id) {
  std::copy(id.clock.begin(), id.clock.end(), p.begin() + at);
  put(p, at + 8, id.port, 2);
}
std::size_t size(Type type) {
  switch (type) {
  case Type::Announce:
    return 64;
  case Type::DelayResp:
    return 54;
  case Type::Sync:
  case Type::FollowUp:
  case Type::DelayReq:
    return 44;
  }
  throw std::runtime_error("Unsupported PTP message type");
}
Time interval(std::int8_t value) {
  return Time(std::ldexp(double(second), std::clamp<int>(value, -3, 4)));
}
std::string clock_id(const std::array<std::uint8_t, 8>& clock) {
  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (const auto v : clock)
    out << std::setw(2) << int(v);
  return out.str();
}
} // namespace
Message decode(std::span<const std::uint8_t> p) {
  if (p.size() < 34 || (p[1] & 15) != 2 || (p[0] & 0xf0) != 0 || get(p, 2, 2) != p.size())
    throw std::runtime_error("Invalid PTPv2 UDP header");
  Message m;
  m.type = static_cast<Type>(p[0] & 15);
  const auto base = size(m.type);
  if (p.size() < base)
    throw std::runtime_error("Truncated PTP message");
  for (auto at = base; at < p.size();) {
    if (p.size() - at < 4)
      throw std::runtime_error("Truncated PTP TLV");
    const auto n = get(p, at + 2, 2);
    if (n % 2 || n > p.size() - at - 4)
      throw std::runtime_error("Invalid PTP TLV length");
    if (get(p, at, 2) == 0x8009)
      throw std::runtime_error("Authenticated PTP profiles are not supported");
    at += 4 + std::size_t(n);
  }
  m.domain = p[4];
  m.flags = std::uint16_t(get(p, 6, 2));
  m.correction_ns = std::bit_cast<Time>(get(p, 8, 8)) / 65536;
  m.source = port(p, 20);
  m.sequence = std::uint16_t(get(p, 30, 2));
  m.interval = std::bit_cast<std::int8_t>(p[33]);
  m.timestamp_ns = stamp(p, 34);
  if (m.type == Type::DelayResp)
    m.requesting = port(p, 44);
  if (m.type == Type::Announce) {
    m.utc_offset = std::bit_cast<std::int16_t>(std::uint16_t(get(p, 44, 2)));
    m.priority1 = p[47];
    m.clock_class = p[48];
    m.accuracy = p[49];
    m.variance = std::uint16_t(get(p, 50, 2));
    m.priority2 = p[52];
    std::copy_n(p.begin() + 53, 8, m.grandmaster.begin());
    m.steps = std::uint16_t(get(p, 61, 2));
    m.time_source = p[63];
  }
  return m;
}
Bytes encode(const Message& m) {
  Bytes p(size(m.type));
  p[0] = static_cast<std::uint8_t>(m.type);
  p[1] = 2;
  put(p, 2, p.size(), 2);
  p[4] = m.domain;
  put(p, 6, m.flags, 2);
  if (m.correction_ns > std::numeric_limits<Time>::max() / 65536 ||
      m.correction_ns < std::numeric_limits<Time>::min() / 65536)
    throw std::runtime_error("PTP correction field overflow");
  put(p, 8, std::bit_cast<std::uint64_t>(m.correction_ns * 65536), 8);
  port(p, 20, m.source);
  put(p, 30, m.sequence, 2);
  p[32] = m.type == Type::Sync        ? 0
          : m.type == Type::DelayReq  ? 1
          : m.type == Type::FollowUp  ? 2
          : m.type == Type::DelayResp ? 3
                                      : 5;
  p[33] = std::bit_cast<std::uint8_t>(m.interval);
  stamp(p, 34, m.timestamp_ns);
  if (m.type == Type::DelayResp)
    port(p, 44, m.requesting);
  if (m.type == Type::Announce) {
    put(p, 44, std::bit_cast<std::uint16_t>(m.utc_offset), 2);
    p[47] = m.priority1;
    p[48] = m.clock_class;
    p[49] = m.accuracy;
    put(p, 50, m.variance, 2);
    p[52] = m.priority2;
    std::copy(m.grandmaster.begin(), m.grandmaster.end(), p.begin() + 53);
    put(p, 61, m.steps, 2);
    p[63] = m.time_source;
  }
  return p;
}
std::string identity(const PortIdentity& p) {
  return clock_id(p.clock) + ":" + std::to_string(p.port);
}
struct Tracker::Impl {
  struct Candidate {
    Message announce;
    std::string host;
    Time seen;
  };
  struct Sync {
    Time origin = 0, arrival = 0, correction = 0;
  };
  struct Pair {
    std::optional<Message> sync, follow;
    Time arrived = 0, seen = 0;
  };
  struct Pending {
    std::uint16_t sequence;
    Time sent;
    Sync sync;
  };
  struct Sample {
    Time time, offset, delay;
  };
  PortIdentity self;
  int domain;
  std::map<PortIdentity, Candidate> candidates;
  std::optional<Candidate> master;
  std::map<std::uint16_t, Pair> pairs;
  std::optional<Sync> sync;
  std::optional<Pending> pending;
  std::deque<Sample> samples;
  Time next_request = 0, request_interval = second;
  std::uint16_t sequence = 0;
  std::uint64_t received = 0, ignored = 0, rejected = 0, requests = 0, completed = 0, timeouts = 0,
                changes = 0, clock_steps = 0;
  explicit Impl(PortIdentity s, int d) : self(s), domain(d) {
    if (d < 0 || d > 255 || !s.port)
      throw std::runtime_error("PTP domain must be 0-255 and port identity must be nonzero");
  }
  auto rank(const Candidate& c) const {
    const auto& m = c.announce;
    return std::tuple{m.priority1, m.clock_class, m.accuracy, m.variance,
                      m.priority2, m.grandmaster, m.steps,    m.source};
  }
  void tick(Time now) {
    std::erase_if(candidates, [&](const auto& item) {
      return now - item.second.seen > 3 * interval(item.second.announce.interval);
    });
    std::optional<Candidate> best;
    for (const auto& [id, c] : candidates)
      if (!best || rank(c) < rank(*best))
        best = c;
    const bool changed =
        bool(best) != bool(master) ||
        (best && master &&
         (best->announce.source != master->announce.source ||
          best->announce.grandmaster != master->announce.grandmaster ||
          best->host != master->host || ((best->announce.flags ^ master->announce.flags) & 0x0c) ||
          best->announce.utc_offset != master->announce.utc_offset));
    if (changed) {
      master = best;
      sync.reset();
      pending.reset();
      pairs.clear();
      samples.clear();
      next_request = now;
      request_interval = second;
      ++changes;
    } else
      master = best;
    std::erase_if(pairs, [&](const auto& item) { return now - item.second.seen > 3 * second; });
    if (pending && now - pending->sent > 2 * second) {
      pending.reset();
      ++timeouts;
    }
  }
  void use_sync(const Sync& s) {
    if (!sync || s.arrival > sync->arrival)
      sync = s;
  }
  void receive(const Message& m, const std::string& host, Time now) {
    ++received;
    if (m.domain != domain || m.source == self || (m.flags & 0x0100)) {
      ++ignored;
      return;
    }
    tick(now);
    if (m.type == Type::Announce) {
      if (m.clock_class == 255 || m.steps >= 255 || m.grandmaster == self.clock) {
        ++ignored;
        return;
      }
      if (candidates.size() >= 32 && !candidates.contains(m.source)) {
        ++rejected;
        return;
      }
      candidates[m.source] = {m, host, now};
      tick(now);
      return;
    }
    if (!master || m.source != master->announce.source || host != master->host) {
      ++ignored;
      return;
    }
    if (m.correction_ns > second || m.correction_ns < -second) {
      ++rejected;
      return;
    }
    if (m.type == Type::Sync || m.type == Type::FollowUp) {
      if (m.type == Type::Sync && !(m.flags & 0x0200)) {
        use_sync({m.timestamp_ns, now, m.correction_ns});
        return;
      }
      if (pairs.size() >= 64 && !pairs.contains(m.sequence)) {
        ++rejected;
        return;
      }
      auto& p = pairs[m.sequence];
      p.seen = now;
      if (m.type == Type::Sync) {
        p.sync = m;
        p.arrived = now;
      } else
        p.follow = m;
      if (p.sync && p.follow) {
        use_sync({p.follow->timestamp_ns, p.arrived,
                  add(p.sync->correction_ns, p.follow->correction_ns)});
        pairs.erase(m.sequence);
      }
      return;
    }
    if (m.type != Type::DelayResp || m.requesting != self || !pending ||
        m.sequence != pending->sequence) {
      ++ignored;
      return;
    }
    const auto request = *pending;
    pending.reset();
    if (now < request.sent || now - request.sent > 2 * second) {
      ++rejected;
      return;
    }
    const Time forward =
                   add(sub(request.sync.origin, request.sync.arrival), request.sync.correction),
               reverse = sub(sub(m.timestamp_ns, request.sent), m.correction_ns);
    const auto difference = sub(reverse, forward);
    if (difference < 0 || difference > 2 * second) {
      ++rejected;
      return;
    }
    const Time offset = std::midpoint(forward, reverse), delay = difference / 2;
    if (!samples.empty() && std::abs(sub(offset, samples.back().offset)) > second) {
      samples.clear();
      ++clock_steps;
    }
    samples.push_back({request.sent, offset, delay});
    if (samples.size() > 32)
      samples.pop_front();
    ++completed;
    if (m.interval != 127)
      request_interval = interval(m.interval);
    next_request = std::max(next_request, add(request.sent, request_interval));
  }
  std::optional<Message> delay_request(Time now) {
    tick(now);
    if (!master || !sync || pending || now < next_request || now - sync->arrival > 3 * second)
      return std::nullopt;
    Message m;
    m.type = Type::DelayReq;
    m.source = self;
    m.domain = std::uint8_t(domain);
    m.sequence = ++sequence;
    m.interval = 127;
    m.timestamp_ns = now;
    if (master->announce.flags & 8)
      m.timestamp_ns = add(m.timestamp_ns, Time(master->announce.utc_offset) * second);
    pending = Pending{m.sequence, now, *sync};
    next_request = add(now, request_interval);
    ++requests;
    return m;
  }
  Json status(Time now) const {
    const bool fresh = master && sync && now - sync->arrival <= 3 * second && !samples.empty() &&
                       now - samples.back().time <= std::max(3 * second, request_interval * 3);
    double frequency = 0, residual = 0;
    bool stable = false;
    if (samples.size() >= 2) {
      double sx = 0, sy = 0, sxx = 0, sxy = 0;
      const auto& ref = samples.front();
      for (const auto& s : samples) {
        const double x = double(s.time - ref.time) / second, y = double(s.offset - ref.offset);
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
      }
      const double n = double(samples.size()), den = n * sxx - sx * sx;
      const double slope = den > 0 ? (n * sxy - sx * sy) / den : 0,
                   intercept = (sy - slope * sx) / n;
      frequency = slope / 1000.;
      for (const auto& s : samples) {
        const double error =
            double(s.offset - ref.offset) - intercept - slope * double(s.time - ref.time) / second;
        residual += error * error;
      }
      residual = std::sqrt(residual / n);
      stable = fresh && samples.size() >= 8 && samples.back().time - ref.time >= 6 * second &&
               residual < 250000 && std::abs(frequency) < 2000;
    }
    const auto state = !master  ? (changes ? "Grandmaster lost" : "Listening")
                       : !sync  ? "Waiting for Sync"
                       : !fresh ? (samples.empty() ? "Measuring" : "PTP data stale")
                       : stable ? "PTP tracking stable"
                                : "Measuring";
    Json out = {{"source", "ptp-monitor"},
                {"mode", "PTPv2 monitor"},
                {"state", state},
                {"locked", false},
                {"synchronized", false},
                {"audioLocked", false},
                {"ptpAvailable", true},
                {"ptpSupported", true},
                {"experimental", true},
                {"trackingStable", stable},
                {"measurementFresh", fresh},
                {"domain", domain},
                {"timestamping", "Application software"},
                {"localPortIdentity", identity(self)},
                {"packets", received},
                {"ignored", ignored},
                {"rejected", rejected},
                {"delayRequests", requests},
                {"samples", completed},
                {"windowSamples", samples.size()},
                {"delayTimeouts", timeouts},
                {"masterChanges", changes},
                {"clockSteps", clock_steps},
                {"frequencyPpm", fresh && samples.size() >= 2 ? Json(frequency) : Json(nullptr)},
                {"residualNs", fresh ? Json(residual) : Json(nullptr)},
                {"offsetNs", nullptr},
                {"rawOffsetNs", nullptr},
                {"pathDelayNs", nullptr},
                {"masterPortIdentity", nullptr},
                {"grandmasterIdentity", nullptr},
                {"masterAddress", nullptr}};
    if (master) {
      out["masterPortIdentity"] = identity(master->announce.source);
      out["grandmasterIdentity"] = clock_id(master->announce.grandmaster);
      out["masterAddress"] = master->host;
      out["ptpTimescale"] = (master->announce.flags & 8) != 0;
      out["utcOffsetValid"] = (master->announce.flags & 4) != 0;
      out["currentUtcOffset"] = master->announce.utc_offset;
    }
    if (!samples.empty()) {
      out["rawOffsetNs"] = samples.back().offset;
      out["pathDelayNs"] = samples.back().delay;
      out["measurementAgeMs"] = double(now - samples.back().time) / 1000000.;
      if (master && (!(master->announce.flags & 8) || (master->announce.flags & 4)))
        out["offsetNs"] =
            sub(samples.back().offset,
                (master->announce.flags & 8) ? Time(master->announce.utc_offset) * second : 0);
    }
    return out;
  }
};
Tracker::Tracker(PortIdentity s, int d) : impl_(std::make_unique<Impl>(s, d)) {}
Tracker::~Tracker() = default;
void Tracker::receive(const Message& m, const std::string& host, Time now) {
  try {
    impl_->receive(m, host, now);
  } catch (const std::exception&) {
    ++impl_->rejected;
  }
}
void Tracker::tick(Time now) {
  impl_->tick(now);
}
std::optional<Message> Tracker::delay_request(Time now) {
  return impl_->delay_request(now);
}
void Tracker::send_failed(std::uint16_t sequence) {
  if (impl_->pending && impl_->pending->sequence == sequence)
    impl_->pending.reset();
}
std::string Tracker::master_address() const {
  return impl_->master ? impl_->master->host : std::string{};
}
Json Tracker::status(Time now) const {
  return impl_->status(now);
}
} // namespace aoip::ptp
