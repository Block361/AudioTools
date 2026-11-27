// SPDX-License-Identifier: MIT
#include "aoip/iot.hpp"
#include "aoip/network.hpp"
#include "httplib.h"
namespace aoip::iot {
Client::Client(std::string address, int port, std::string token, std::string id)
    : address_(std::move(address)), token_(std::move(token)), id_(std::move(id)), port_(port) {
  if (!valid_ipv4(address_) || port < 1024 || port > 65535 || token_.size() != 64 || !valid_id(id_))
    throw std::runtime_error("Select an IoT device and enter its 64-character management key");
}
Json Client::request(const std::string& method, const std::string& path, const Json& body) const {
  httplib::Client client(address_, port_);
  client.set_connection_timeout(2, 0);
  client.set_read_timeout(10, 0);
  client.set_write_timeout(3, 0);
  client.set_follow_location(false);
  client.set_default_headers({{"Authorization", "Bearer " + token_}});
  auto result =
      method == "GET" ? client.Get(path) : client.Post(path, body.dump(), "application/json");
  if (!result)
    throw std::runtime_error(
        "IoT device unreachable; check its address, service and management port");
  if (result->body.size() > 256 * 1024)
    throw std::runtime_error("IoT response exceeds the supported size");
  const auto value = Json::parse(result->body);
  if (result->status != 200)
    throw std::runtime_error(value.value("error", "IoT request failed"));
  if (value.value("product", "") != "OpenAoIP IoT" || value.value("id", "") != id_ ||
      value.value("schemaVersion", 0) != protocol_version)
    throw std::runtime_error(
        "IoT response identity or API version does not match the selected device");
  return value;
}
Json Client::configuration() const {
  return request("GET", "/v1/config");
}
Json Client::inventory() const {
  return request("GET", "/v1/audio-devices");
}
Json Client::status() const {
  return request("GET", "/v1/status");
}
Json Client::configure(std::uint64_t revision, const Json& audio) const {
  return request("POST", "/v1/config", {{"revision", revision}, {"audio", audio}});
}
Json Client::set_running(std::uint64_t revision, bool running) const {
  return request("POST", "/v1/run", {{"revision", revision}, {"running", running}});
}
} // namespace aoip::iot
