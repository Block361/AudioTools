#include "aoip/discovery.hpp"
#include "aoip/network.hpp"
#include "json.hpp"
#include <atomic>
#include <chrono>
#include <set>
#include <thread>
namespace aoip {
using Json = nlohmann::json;
struct DiscoveryServer::Impl {
  UdpSocket socket;
  std::atomic<bool> running{true};
  std::thread worker;
  std::string id = random_id();
  int http_port;
  Impl(int p, const std::string& host, int udp) : socket(udp, host), http_port(p) {
    worker = std::thread([this] {
      while (running) {
        try {
          for (int i = 0; i < 32; ++i) {
            auto packet = socket.receive_from();
            if (!packet)
              break;
            if (packet->data.size() > 1024)
              continue;
            const auto request = Json::parse(packet->data, nullptr, false);
            if (!request.is_object() ||
                request.value("protocol", Json()) != "OpenAoIP.Discovery.1" ||
                request.value("type", Json()) != "find-controller")
              continue;
            const auto text = Json{
                {"protocol", "OpenAoIP.Discovery.1"},
                {"type", "controller"},
                {"id", id},
                {"name", "OpenAoIP Controller"},
                {"port",
                 http_port}}.dump();
            socket.send(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()),
                        packet->host, packet->port);
          }
        } catch (...) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    });
  }
  ~Impl() {
    running = false;
    if (worker.joinable())
      worker.join();
  }
};
DiscoveryServer::DiscoveryServer(int port, const std::string& host, int udp)
    : impl_(std::make_unique<Impl>(port, host, udp)) {}
DiscoveryServer::~DiscoveryServer() = default;
std::vector<DiscoveredController> discover_controllers(const std::string& host, int timeout_ms,
                                                       int port) {
  const auto query = Json{{"protocol", "OpenAoIP.Discovery.1"}, {"type", "find-controller"}}.dump();
  std::vector<std::unique_ptr<UdpSocket>> sockets;
  auto hosts = host == "0.0.0.0" ? local_addresses() : std::vector<std::string>{host};
  if (host == "0.0.0.0")
    hosts.push_back("127.0.0.1");
  for (const auto& ip : hosts)
    try {
      auto socket = std::make_unique<UdpSocket>(0, ip);
      socket->enable_broadcast();
      const auto data =
          std::span(reinterpret_cast<const std::uint8_t*>(query.data()), query.size());
      socket->send(data, "255.255.255.255", port);
      socket->send(data, "127.0.0.1", port);
      sockets.push_back(std::move(socket));
    } catch (...) {
    }
  std::vector<DiscoveredController> found;
  std::set<std::string> seen;
  const auto end = monotonic_ms() + timeout_ms;
  while (monotonic_ms() < end) {
    for (auto& socket : sockets)
      for (int i = 0; i < 16; ++i) {
        auto packet = socket->receive_from();
        if (!packet)
          break;
        if (packet->data.size() > 2048)
          continue;
        const auto reply = Json::parse(packet->data, nullptr, false);
        if (!reply.is_object() || reply.value("protocol", Json()) != "OpenAoIP.Discovery.1" ||
            reply.value("type", Json()) != "controller")
          continue;
        try {
          const int p = reply.at("port").get<int>();
          if (p < 1 || p > 65535)
            continue;
          const auto id = reply.at("id").get<std::string>();
          const auto url = "http://" + packet->host + ":" + std::to_string(p);
          if (seen.insert(id).second)
            found.push_back({id, reply.value("name", "OpenAoIP Controller"), packet->host, url, p});
        } catch (...) {
        }
      }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return found;
}
} // namespace aoip
