#pragma once
#include <memory>
#include <string>
#include <vector>
namespace aoip {
struct DiscoveredController {
  std::string id, name, host, url;
  int port;
};
class DiscoveryServer {
public:
  DiscoveryServer(int http_port, const std::string& host = "0.0.0.0", int discovery_port = 32100);
  ~DiscoveryServer();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
std::vector<DiscoveredController> discover_controllers(const std::string& bind_host = "0.0.0.0",
                                                       int timeout_ms = 600, int port = 32100);
} // namespace aoip
