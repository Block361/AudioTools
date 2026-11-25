#ifdef _WIN32
// Winsock must precede Windows headers, and multimedia declarations follow them.
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <mmsystem.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
// clang-format on
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <unistd.h>
#endif
#include "aoip/network.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <random>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace aoip {
namespace {
#ifdef _WIN32
using Handle = SOCKET;
constexpr auto invalid_socket = INVALID_SOCKET;
void close_socket(Handle socket) {
  closesocket(socket);
}
#else
using Handle = int;
constexpr auto invalid_socket = -1;
void close_socket(Handle socket) {
  close(socket);
}
#endif
sockaddr_in address(const std::string& host, int port) {
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<std::uint16_t>(port));
  if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) != 1)
    throw std::runtime_error("Expected an IPv4 address: " + host);
  return a;
}
} // namespace
NetworkRuntime::NetworkRuntime() {
#ifdef _WIN32
  WSADATA data{};
  if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
    throw std::runtime_error("WSAStartup failed");
  timeBeginPeriod(1);
#endif
}
NetworkRuntime::~NetworkRuntime() {
#ifdef _WIN32
  timeEndPeriod(1);
  WSACleanup();
#endif
}
struct UdpSocket::Impl {
  Handle socket = invalid_socket;
  int port{};
  ~Impl() {
    if (socket != invalid_socket)
      close_socket(socket);
  }
};
UdpSocket::UdpSocket(int port, const std::string& host, bool exclusive)
    : impl_(std::make_unique<Impl>()) {
  impl_->socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (impl_->socket == invalid_socket)
    throw std::runtime_error("Cannot create UDP socket");
#ifdef _WIN32
  if (exclusive) {
    const int enabled = 1;
    if (setsockopt(impl_->socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                   reinterpret_cast<const char*>(&enabled), sizeof(enabled)) != 0)
      throw std::runtime_error("Cannot claim exclusive UDP socket ownership");
  }
#else
  (void)exclusive; // No SO_REUSEADDR/PORT is enabled, so UDP binding remains exclusive.
#endif
  const auto a = address(host, port);
  if (::bind(impl_->socket, reinterpret_cast<const sockaddr*>(&a), sizeof(a)) != 0)
    throw std::runtime_error("Cannot bind UDP " + host + ":" + std::to_string(port));
  sockaddr_in actual{};
#ifdef _WIN32
  int length = sizeof(actual);
  u_long nonblocking = 1;
  if (ioctlsocket(impl_->socket, FIONBIO, &nonblocking) != 0)
    throw std::runtime_error("Cannot configure UDP");
#else
  socklen_t length = sizeof(actual);
  if (fcntl(impl_->socket, F_SETFL, O_NONBLOCK) < 0)
    throw std::runtime_error("Cannot configure UDP");
#endif
  if (getsockname(impl_->socket, reinterpret_cast<sockaddr*>(&actual), &length) != 0)
    throw std::runtime_error("Cannot inspect UDP socket");
  impl_->port = ntohs(actual.sin_port);
  int size = 1024 * 1024;
  setsockopt(impl_->socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&size),
             sizeof(size));
}
UdpSocket::~UdpSocket() = default;
UdpSocket::UdpSocket(UdpSocket&&) noexcept = default;
UdpSocket& UdpSocket::operator=(UdpSocket&&) noexcept = default;
int UdpSocket::port() const {
  return impl_->port;
}
bool UdpSocket::send(std::span<const std::uint8_t> bytes, const std::string& host, int port) {
  const auto a = address(host, port);
  return sendto(impl_->socket, reinterpret_cast<const char*>(bytes.data()),
                static_cast<int>(bytes.size()), 0, reinterpret_cast<const sockaddr*>(&a),
                sizeof(a)) == static_cast<int>(bytes.size());
}
std::optional<Bytes> UdpSocket::receive() {
  auto packet = receive_from();
  if (!packet)
    return std::nullopt;
  return std::move(packet->data);
}
void UdpSocket::enable_broadcast() {
  int enabled = 1;
  if (setsockopt(impl_->socket, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&enabled),
                 sizeof(enabled)) != 0)
    throw std::runtime_error("Cannot enable network discovery broadcast");
}
void UdpSocket::join_multicast(const std::string& group, const std::string& interface_ip) {
  ip_mreq membership{};
  if (inet_pton(AF_INET, group.c_str(), &membership.imr_multiaddr) != 1 ||
      inet_pton(AF_INET, interface_ip.c_str(), &membership.imr_interface) != 1)
    throw std::runtime_error("Invalid IPv4 multicast interface");
  if (setsockopt(impl_->socket, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                 reinterpret_cast<const char*>(&membership), sizeof(membership)) != 0 ||
      setsockopt(impl_->socket, IPPROTO_IP, IP_MULTICAST_IF,
                 reinterpret_cast<const char*>(&membership.imr_interface),
                 sizeof(membership.imr_interface)) != 0)
    throw std::runtime_error("Cannot join PTP multicast on the selected interface");
  const int ttl = 1, loop = 0;
  if (setsockopt(impl_->socket, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<const char*>(&ttl),
                 sizeof(ttl)) != 0 ||
      setsockopt(impl_->socket, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<const char*>(&loop),
                 sizeof(loop)) != 0)
    throw std::runtime_error("Cannot configure PTP multicast transport");
}
std::optional<UdpSocket::Datagram> UdpSocket::receive_from() {
  // A full datagram buffer lets malformed oversized packets be rejected by the
  // RTP decoder without making Windows recvfrom fail with WSAEMSGSIZE.
  std::array<std::uint8_t, 65536> bytes;
  sockaddr_in source{};
#ifdef _WIN32
  int source_size = sizeof(source);
#else
  socklen_t source_size = sizeof(source);
#endif
  const int length = static_cast<int>(recvfrom(impl_->socket, reinterpret_cast<char*>(bytes.data()),
                                               static_cast<int>(bytes.size()), 0,
                                               reinterpret_cast<sockaddr*>(&source), &source_size));
  if (length < 0) {
#ifdef _WIN32
    const auto error = WSAGetLastError();
    if (error == WSAEWOULDBLOCK || error == WSAEINTR || error == WSAECONNRESET ||
        error == WSAENETRESET)
      return std::nullopt;
#else
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ECONNREFUSED)
      return std::nullopt;
#endif
    throw std::runtime_error("UDP receive failed");
  }
  const auto arrived = monotonic_ns();
  char host[INET_ADDRSTRLEN]{};
  inet_ntop(AF_INET, &source.sin_addr, host, sizeof(host));
  return Datagram{Bytes(bytes.begin(), bytes.begin() + length), host, ntohs(source.sin_port),
                  arrived};
}
std::int64_t monotonic_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
double monotonic_ms() {
  return std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
std::uint32_t random_uint32() {
  thread_local std::mt19937 engine(std::random_device{}());
  return engine();
}
std::string random_id() {
  std::ostringstream out;
  out << std::hex << random_uint32() << random_uint32();
  return out.str();
}
bool valid_id(const std::string& id) {
  if (id.empty() || id.size() > 48)
    return false;
  for (const unsigned char c : id)
    if (!std::isalnum(c) && c != '-' && c != '_')
      return false;
  return true;
}
bool valid_ipv4(const std::string& host) {
  in_addr a{};
  return inet_pton(AF_INET, host.c_str(), &a) == 1;
}
std::vector<std::string> local_addresses() {
  std::vector<std::string> result;
  std::vector<std::string> preferred;
  auto add = [&](const sockaddr* socket_address, bool has_gateway = false) {
    if (!socket_address || socket_address->sa_family != AF_INET)
      return;
    const auto* address = reinterpret_cast<const sockaddr_in*>(socket_address);
    char text[INET_ADDRSTRLEN]{};
    if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)) &&
        !std::string(text).starts_with("127.") && std::string(text) != "0.0.0.0")
      (has_gateway ? preferred : result).emplace_back(text);
  };
#ifdef _WIN32
  ULONG bytes = 15000;
  for (int attempt = 0; attempt < 3; ++attempt) {
    std::vector<std::uint64_t> buffer((bytes + 7) / 8);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
    const auto status =
        GetAdaptersAddresses(AF_INET,
                             GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                 GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS,
                             nullptr, adapters, &bytes);
    if (status == ERROR_BUFFER_OVERFLOW)
      continue;
    if (status == ERROR_NO_DATA)
      return result;
    if (status != NO_ERROR)
      throw std::runtime_error("Cannot enumerate network interfaces");
    for (auto* adapter = adapters; adapter; adapter = adapter->Next) {
      if (adapter->OperStatus != IfOperStatusUp)
        continue;
      for (auto* address = adapter->FirstUnicastAddress; address; address = address->Next)
        add(address->Address.lpSockaddr, adapter->FirstGatewayAddress != nullptr);
    }
    break;
  }
#else
  ifaddrs* interfaces = nullptr;
  if (getifaddrs(&interfaces) != 0)
    throw std::runtime_error("Cannot enumerate network interfaces");
  for (auto* item = interfaces; item; item = item->ifa_next)
    if ((item->ifa_flags & IFF_UP) && !(item->ifa_flags & IFF_LOOPBACK))
      add(item->ifa_addr);
  freeifaddrs(interfaces);
#endif
  // Prefer a routed adapter to virtual/host-only adapters, retaining OS adapter order.
  preferred.insert(preferred.end(), result.begin(), result.end());
  result.clear();
  for (const auto& ip : preferred)
    if (std::find(result.begin(), result.end(), ip) == result.end())
      result.push_back(ip);
  return result;
}
std::string local_control_host(const std::string& bind_host) {
  if (bind_host != "0.0.0.0")
    return bind_host;
  const auto addresses = local_addresses();
  return addresses.empty() ? "127.0.0.1" : addresses.front();
}
} // namespace aoip
