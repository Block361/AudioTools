#pragma once
#include "aoip/system.hpp"
#ifdef _WIN32
namespace aoip::ptp {
class Monitor;
}
namespace aoip::vsc {
struct Settings {
  std::string id = "vsc-1", name = "OpenAoIP Virtual Soundcard", interface_ip = "0.0.0.0",
              controller = "auto", backend = "asio", clock_source = "internal";
  int tx_channels = 8, rx_channels = 8, rate = 48000, latency_ms = 60, buffer_frames = 256;
  int ptp_domain = 0;
  std::vector<std::string> tx_names, rx_names;
  std::wstring bridge_mapping = L"Local\\OpenAoIP.VirtualSoundcard.Audio.v1";
};
Json to_json(const Settings&);
Settings from_json(const Json&);
class Engine {
public:
  explicit Engine(Settings settings, std::shared_ptr<ptp::Monitor> clock = {});
  ~Engine();
  Json status() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace aoip::vsc
#endif
