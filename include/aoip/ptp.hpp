// SPDX-License-Identifier: MIT
#pragma once
#include "aoip/network.hpp"
#include "json.hpp"
#include <array>
#include <compare>
#include <memory>
namespace aoip::ptp {
using Json = nlohmann::json;
using Time = std::int64_t;
enum class Type : std::uint8_t {
  Sync = 0,
  DelayReq = 1,
  FollowUp = 8,
  DelayResp = 9,
  Announce = 11
};
struct PortIdentity {
  std::array<std::uint8_t, 8> clock{};
  std::uint16_t port = 1;
  auto operator<=>(const PortIdentity&) const = default;
};
struct Message {
  Type type = Type::Sync;
  std::uint8_t domain = 0;
  PortIdentity source, requesting;
  std::uint16_t sequence = 0, flags = 0;
  std::int8_t interval = 0;
  Time correction_ns = 0, timestamp_ns = 0;
  std::int16_t utc_offset = 0;
  std::uint8_t priority1 = 128, clock_class = 248, accuracy = 0xfe, priority2 = 128,
               time_source = 0xa0;
  std::uint16_t variance = 0xffff, steps = 0;
  std::array<std::uint8_t, 8> grandmaster{};
};
Message decode(std::span<const std::uint8_t>);
Bytes encode(const Message&);
std::string identity(const PortIdentity&);

// Protocol/measurement state only. All timestamps use one monotonic, UTC-anchored session clock.
// It never disciplines the Windows clock or claims that the audio clock is locked.
class Tracker {
public:
  Tracker(PortIdentity self, int domain);
  ~Tracker();
  void receive(const Message&, const std::string& source_ip, Time arrival);
  void tick(Time now);
  std::optional<Message> delay_request(Time now);
  void send_failed(std::uint16_t sequence);
  std::string master_address() const;
  Json status(Time now) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
enum class Role { Monitor, Grandmaster };
struct Options {
  std::string interface_ip;
  Role role = Role::Monitor;
  int domain = 0;
  int event_port = 319, general_port = 320, destination_event_port = 319,
      destination_general_port = 320;
  bool multicast = true; // Non-multicast/ephemeral ports are used only by explicit transport tests.
};
class Monitor {
public:
  explicit Monitor(Options);
  ~Monitor();
  Json status() const;
  int event_port() const;
  int general_port() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
// One selected interface/domain per process in this first integration; Bridge and VSC share it.
std::shared_ptr<Monitor> shared_monitor(const std::string& interface_ip, int domain,
                                        Role role = Role::Monitor);
std::shared_ptr<Monitor> shared_monitor(Options options);
} // namespace aoip::ptp
