#pragma once
#include "aoip/audio.hpp"
#include <memory>
#include <string>

namespace aoip {
class NetworkRuntime {
public:
  NetworkRuntime();
  ~NetworkRuntime();
  NetworkRuntime(const NetworkRuntime&) = delete;
  NetworkRuntime& operator=(const NetworkRuntime&) = delete;
};
class UdpSocket {
public:
  struct Datagram {
    Bytes data;
    std::string host;
    int port;
    std::int64_t received_ns = 0;
  };
  explicit UdpSocket(int port = 0, const std::string& host = "0.0.0.0", bool exclusive = false);
  ~UdpSocket();
  UdpSocket(UdpSocket&&) noexcept;
  UdpSocket& operator=(UdpSocket&&) noexcept;
  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;
  bool send(std::span<const std::uint8_t> bytes, const std::string& host, int port);
  std::optional<Bytes> receive();
  std::optional<Datagram> receive_from();
  void enable_broadcast();
  void join_multicast(const std::string& group, const std::string& interface_ip);
  int port() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
double monotonic_ms();
std::int64_t monotonic_ns();
std::uint32_t random_uint32();
std::string random_id();
bool valid_id(const std::string& id);
bool valid_ipv4(const std::string& host);
std::vector<std::string> local_addresses();
std::string local_control_host(const std::string& bind_host);
} // namespace aoip
