// SPDX-License-Identifier: MIT
#pragma once
#include "aoip/bridge_engine.hpp"
#include <filesystem>
namespace aoip::iot {
inline constexpr int protocol_version = 1;
inline constexpr char version[] = "0.1.0";
struct Configuration {
  bridge::Settings audio;
  std::string bind = "0.0.0.0", token;
  int port = 32110;
  bool auto_start = false;
  std::uint64_t revision = 1;
};
Configuration initial_configuration();
Configuration configuration_from_json(const Json&);
Json to_json(const Configuration&, bool include_secret = false);
Json audio_inventory(AudioBackend backend = AudioBackend::Alsa);
class Host {
public:
  explicit Host(std::filesystem::path configuration, AudioBackend backend = AudioBackend::Alsa);
  ~Host();
  int port() const;
  Json status() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
// Controller-side client. Keys remain in caller memory; they are never advertised or exported.
class Client {
public:
  Client(std::string address, int port, std::string token, std::string expected_id);
  Json configuration() const;
  Json inventory() const;
  Json status() const;
  Json configure(std::uint64_t revision, const Json& audio) const;
  Json set_running(std::uint64_t revision, bool running) const;

private:
  std::string address_, token_, id_;
  int port_;
  Json request(const std::string& method, const std::string& path, const Json& body = {}) const;
};
} // namespace aoip::iot
