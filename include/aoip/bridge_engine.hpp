#pragma once
#include "aoip/audio_endpoint.hpp"
#include "aoip/session.hpp"
namespace aoip::bridge {
struct Patch {
  int local = 1, network = 1;
  float gain = 1.f;
};
struct Endpoint {
  std::string id, name, device = "default";
  EndpointMode mode = EndpointMode::Capture;
  std::vector<Patch> patches;
};
struct Settings {
  std::string id = "bridge-1", name = "OpenAoIP Audio Bridge", controller = "auto",
              interface_ip = "0.0.0.0", clock_source = "internal";
  int tx_channels = 8, rx_channels = 8, rate = 48000, buffer_frames = 256, latency_ms = 60,
      local_latency_ms = 20;
  int ptp_domain = 0;
  std::vector<std::string> tx_names, rx_names;
  std::vector<Endpoint> endpoints;
};
Json to_json(const Settings&);
Settings from_json(const Json&);
void validate(const Settings&, bool require_endpoints = true);
class Engine {
public:
  explicit Engine(Settings, AudioBackend backend = AudioBackend::System,
                  std::shared_ptr<ptp::Monitor> clock = {}, bool register_with_controller = true);
  ~Engine();
  Json status() const;
  void apply_plan(const Json&);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace aoip::bridge
