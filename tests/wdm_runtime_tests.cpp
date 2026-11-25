#include "aoip/network.hpp"
#include "aoip/vsc_engine.hpp"
#include "aoip/wdm_client.hpp"
#include <cmath>
#include <iostream>
#include <thread>
#include <windows.h>
using namespace aoip;
int find(const std::vector<AudioDeviceInfo>& list, const std::string& suffix) {
  for (const auto& d : list)
    if (d.name.find("OpenAoIP") != std::string::npos && d.name.find(suffix) != std::string::npos)
      return d.index;
  throw std::runtime_error("Expected Windows endpoint missing: " + suffix);
}
double frequency(const Bytes& pcm) {
  const auto frames = std::min<std::size_t>(pcm.size() / 4, 24000);
  if (frames < 16000)
    return 0;
  int crossings = 0, previous = 0;
  for (auto f = pcm.size() / 4 - frames; f < pcm.size() / 4; ++f) {
    const auto raw = (int(pcm[f * 4]) << 8) | pcm[f * 4 + 1];
    const auto value = raw >= 32768 ? raw - 65536 : raw;
    if (previous < 0 && value >= 0)
      ++crossings;
    previous = value;
  }
  return crossings * 48000. / frames;
}
int main() {
  try {
    NetworkRuntime runtime;
    const auto probe = vsc::wdm_probe();
    if (!probe.value("available", false)) {
      std::cout << "SKIP: signed OpenAoIP Windows audio driver is not installed; real "
                   "WASAPI/WaveRT transport was not tested.\n";
      return 77;
    }
    const auto devices = audio_devices();
    const auto output_a = std::to_string(find(devices.playback, "Senden 01/02")),
               output_b = std::to_string(find(devices.playback, "Senden 31/32"));
    const auto input_a = std::to_string(find(devices.capture, "Empfang 01/02")),
               input_b = std::to_string(find(devices.capture, "Empfang 31/32"));
    ControllerConfig cc;
    cc.port = 0;
    cc.state_file = "";
    Controller controller(cc);
    vsc::Settings settings;
    settings.id = "wdm-runtime-test";
    settings.backend = "wdm";
    settings.tx_channels = settings.rx_channels = 32;
    settings.controller = "http://127.0.0.1:" + std::to_string(controller.port());
    settings.interface_ip = "127.0.0.1";
    settings.bridge_mapping =
        L"Local\\OpenAoIP.Test.WdmRuntime." + std::to_wstring(GetCurrentProcessId());
    vsc::Engine engine(settings);
    CaptureDevice capture_a(input_a, 1), capture_b(input_b, 1);
    PlaybackDevice playback_a(output_a, 1, {}, 1.f), playback_b(output_b, 1, {}, 1.f);
    const auto registered = monotonic_ms() + 8000;
    while (controller.state().at("devices").empty() && monotonic_ms() < registered)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    for (const auto& mapping : std::vector<std::pair<int, int>>{{1, 16}, {16, 1}})
      controller.set_route({{"sender", settings.id},
                            {"stream", "stream-" + std::to_string(mapping.first)},
                            {"receiver", settings.id},
                            {"input", "input-" + std::to_string(mapping.second)}});
    capture_a.start();
    capture_b.start();
    playback_a.start();
    playback_b.start();
    std::jthread producer([&](std::stop_token stop) {
      std::uint64_t at = 0;
      auto deadline = std::chrono::steady_clock::now();
      while (!stop.stop_requested()) {
        std::array<Pcm, 1> a{tone_packet(at, 440)}, b{tone_packet(at, 660)};
        playback_a.write(a);
        playback_b.write(b);
        at += frames_per_packet;
        deadline += std::chrono::milliseconds(5);
        std::this_thread::sleep_until(deadline);
      }
    });
    Bytes a, b;
    const auto end = monotonic_ms() + 15000;
    while (monotonic_ms() < end) {
      std::array<Pcm, 1> packet{};
      while (capture_a.read(packet))
        a.insert(a.end(), packet[0].begin(), packet[0].end());
      while (capture_b.read(packet))
        b.insert(b.end(), packet[0].begin(), packet[0].end());
      if (std::abs(frequency(a) - 660) < 20 && std::abs(frequency(b) - 440) < 20) {
        std::cout << "PASS real Windows playback endpoints -> WaveRT -> WDM bridge -> RTP/UDP "
                     "routing -> independent Windows recording endpoints (pairs 01/02 and 31/32)\n";
        return 0;
      }
      if (a.size() > 480000)
        a.erase(a.begin(), a.end() - 240000);
      if (b.size() > 480000)
        b.erase(b.begin(), b.end() - 240000);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("Real Windows audio round trip failed; verify driver/kernel logs, "
                             "formats and endpoint enumeration");
  } catch (const std::exception& e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
