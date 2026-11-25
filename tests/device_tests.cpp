#include "aoip/audio.hpp"
#include "aoip/network.hpp"
#include "aoip/system.hpp"
#include "httplib.h"
#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <thread>

using namespace aoip;
using namespace std::chrono_literals;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x))                                                                                      \
      throw std::runtime_error(std::string("Check failed at ") + std::to_string(__LINE__) + ": " + \
                               #x);                                                                \
  } while (false)
template <class F> void eventually(F condition) {
  const auto end = monotonic_ms() + 10000;
  do {
    if (condition())
      return;
    std::this_thread::sleep_for(25ms);
  } while (monotonic_ms() < end);
  throw std::runtime_error("Network device condition timed out");
}
Json registered(const Controller& c, const std::string& id) {
  const auto state = c.state();
  for (const auto& device : state.at("devices"))
    if (device.at("id") == id)
      return device;
  return nullptr;
}
Json route(const std::string& source, int stream, const std::string& target, int input) {
  return {{"sender", source},
          {"stream", "stream-" + std::to_string(stream)},
          {"receiver", target},
          {"input", "input-" + std::to_string(input)}};
}
double tone_frequency(const std::string& wav) {
  const auto pcm =
      decode_wav(std::span(reinterpret_cast<const std::uint8_t*>(wav.data()), wav.size()));
  const auto frames = std::min<std::size_t>(24000, pcm.size() / 4);
  int crossings = 0, previous = 0;
  for (std::size_t i = pcm.size() / 4 - frames; i < pcm.size() / 4; ++i) {
    const int raw = (int(pcm[i * 4]) << 8) | pcm[i * 4 + 1];
    const int value = raw >= 32768 ? raw - 65536 : raw;
    if (previous < 0 && value >= 0)
      ++crossings;
    previous = value;
  }
  return crossings * 48000.0 / frames;
}
void multiple_device_test() {
  const auto file =
      (std::filesystem::current_path() / ("duplex-test-" + random_id() + ".json")).string();
  struct Cleanup {
    std::string file;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove(file, error);
      std::filesystem::remove(file + ".tmp", error);
    }
  } cleanup{file};
  ControllerConfig cc;
  cc.port = 0;
  cc.state_file = file;
  auto controller = std::make_unique<Controller>(cc);
  const int port = controller->port();
  const auto url = "http://127.0.0.1:" + std::to_string(port);
  std::vector<DeviceConfig> configs;
  std::vector<std::unique_ptr<NetworkDevice>> peers;
  for (int i = 0; i < 3; ++i) {
    DeviceConfig c;
    c.id = "rack-" + std::to_string(i + 1);
    c.name = c.id;
    c.controller = url;
    c.host = "127.0.0.1";
    c.sender.streams = 2;
    c.sender.tone = 440 + i * 220;
    c.receiver.inputs = 2;
    c.receiver.port = 0;
    configs.push_back(c);
    peers.push_back(std::make_unique<NetworkDevice>(c));
  }
  eventually([&] { return controller->state().at("devices").size() == 3; });
  for (const auto& c : configs) {
    const auto d = registered(*controller, c.id);
    CHECK(d.at("role") == "device");
    CHECK(d.at("streams").size() == 2 && d.at("inputs").size() == 2);
    CHECK(d.at("host") == "127.0.0.1");
  }
  controller->set_route(route("rack-1", 1, "rack-2", 1));
  controller->set_route(route("rack-2", 1, "rack-1", 1));
  controller->set_route(route("rack-1", 2, "rack-3", 1));
  controller->set_route(route("rack-3", 1, "rack-1", 2));
  controller->set_route(route("rack-2", 2, "rack-3", 2));
  controller->set_route(route("rack-1", 1, "rack-2", 2));
  auto all_receiving = [&] {
    for (const auto& c : configs) {
      const auto d = registered(*controller, c.id);
      if (d.is_null() || !d.at("online").get<bool>())
        return false;
      for (const auto& input : d.at("inputs"))
        if (input.at("stats").value("capturedSeconds", 0.0) < 0.5)
          return false;
    }
    return true;
  };
  eventually(all_receiving);
  httplib::Client http("127.0.0.1", port);
  http.set_connection_timeout(1, 0);
  http.set_read_timeout(3, 0);
  const auto audio = [&](const std::string& id, int input, double frequency) {
    // Heartbeat telemetry can precede a newly applied plan. Wait for the current
    // receiver capture itself, rather than treating a cached status as a barrier.
    eventually([&] {
      const auto result = http.Get("/api/audio/" + id + "/input-" + std::to_string(input) + ".wav");
      if (!result || result->status == 404 || result->status == 409)
        return false;
      CHECK(result->status == 200);
      if (result->body.size() < 96044)
        return false;
      return std::abs(tone_frequency(result->body) - frequency) < 20;
    });
  };
  audio("rack-1", 1, 660);
  audio("rack-1", 2, 880);
  audio("rack-2", 1, 440);
  audio("rack-2", 2, 440);
  audio("rack-3", 1, 550);
  audio("rack-3", 2, 770);
  auto network = http.Get("/api/network");
  CHECK(network && network->status == 200);
  CHECK(Json::parse(network->body).at("port") == port);
  // A competing device must neither take over nor unregister a live instance.
  const auto instance = registered(*controller, "rack-1").at("instance");
  {
    NetworkDevice duplicate(configs[0]);
    std::this_thread::sleep_for(150ms);
  }
  CHECK(registered(*controller, "rack-1").at("instance") == instance);
  CHECK(registered(*controller, "rack-1").at("online").get<bool>());
  // Graceful goodbye makes the ID available immediately and saved routes reconnect.
  peers[0].reset();
  CHECK(!registered(*controller, "rack-1").at("online").get<bool>());
  peers[0] = std::make_unique<NetworkDevice>(configs[0]);
  eventually([&] {
    return registered(*controller, "rack-1").at("instance") != instance && all_receiving();
  });
  audio("rack-2", 1, 440);
  // Controller outage must not stop device-to-device audio.
  const auto received =
      peers[1]->describe().at("inputs").at(0).at("stats").at("received").get<std::uint64_t>();
  controller.reset();
  std::this_thread::sleep_for(650ms);
  CHECK(peers[1]->describe().at("inputs").at(0).at("stats").at("received").get<std::uint64_t>() >
        received + 50);
  cc.port = port;
  controller = std::make_unique<Controller>(cc);
  CHECK(controller->state().at("routes").size() == 6);
  eventually(all_receiving);
  audio("rack-3", 2, 770);
  // Send-only and receive-only devices use the same registration protocol.
  DeviceConfig tx = configs[0];
  tx.id = "send-only";
  tx.name = tx.id;
  tx.sender.streams = 1;
  tx.sender.tone = 990;
  tx.receiver.inputs = 0;
  DeviceConfig rx = configs[0];
  rx.id = "receive-only";
  rx.name = rx.id;
  rx.sender.streams = 0;
  rx.receiver.inputs = 1;
  NetworkDevice source(tx), target(rx);
  eventually([&] {
    return !registered(*controller, tx.id).is_null() && !registered(*controller, rx.id).is_null();
  });
  controller->set_route(route(tx.id, 1, rx.id, 1));
  eventually([&] {
    return registered(*controller, rx.id)
               .at("inputs")
               .at(0)
               .at("stats")
               .value("capturedSeconds", 0.0) > 0.5;
  });
  audio(rx.id, 1, 990);
}
int main() {
  try {
    NetworkRuntime runtime;
    multiple_device_test();
    std::cout << "PASS 3 duplex devices, 6 simultaneous routes, bidirectional waveforms, fan-out, "
                 "duplicate IDs, re-registration and controller recovery\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
