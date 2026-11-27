// SPDX-License-Identifier: MIT
#include "aoip/config_store.hpp"
#include "aoip/iot.hpp"
#include "aoip/network.hpp"
#include "httplib.h"
#include <iostream>
#include <thread>
using namespace aoip;
using namespace std::chrono_literals;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x))                                                                                      \
      throw std::runtime_error("Check failed: " #x);                                               \
  } while (false)
template <class F> void rejects(F f) {
  bool rejected = false;
  try {
    f();
  } catch (const std::exception&) {
    rejected = true;
  }
  CHECK(rejected);
}
template <class F> void eventually(F f) {
  const auto end = monotonic_ms() + 10000;
  do {
    if (f())
      return;
    std::this_thread::sleep_for(20ms);
  } while (monotonic_ms() < end);
  throw std::runtime_error("IoT integration timed out");
}
Json device(const Controller& c, const std::string& id) {
  const auto state = c.state();
  for (const auto& d : state.at("devices"))
    if (d.at("id") == id)
      return d;
  return nullptr;
}
int main(int argc, char** argv) {
  try {
    NetworkRuntime network;
    if (argc == 4 && std::string(argv[1]) == "--serve-fixture") {
      const std::filesystem::path root = argv[2];
      std::filesystem::create_directories(root);
      auto cfg = iot::initial_configuration();
      cfg.bind = "127.0.0.1";
      cfg.port = 0;
      cfg.audio.interface_ip = "127.0.0.1";
      cfg.audio.controller = argv[3];
      write_configuration(root / "iot.json", iot::to_json(cfg, true));
      iot::Host host(root / "iot.json", AudioBackend::TestNull);
      write_configuration(root / "ready.json",
                          {{"id", cfg.audio.id}, {"port", host.port()}, {"token", cfg.token}});
      while (!std::filesystem::exists(root / "stop"))
        std::this_thread::sleep_for(100ms);
      return 0;
    }
    ControllerConfig cc;
    cc.port = 0;
    cc.state_file = "";
    Controller controller(cc);
    const auto url = "http://127.0.0.1:" + std::to_string(controller.port());
    const auto root = std::filesystem::temp_directory_path() / ("OpenAoIP-iot-test-" + random_id());
    std::filesystem::create_directories(root);
    const auto file = root / "iot.json";
    auto cfg = iot::initial_configuration();
    cfg.bind = "127.0.0.1";
    cfg.port = 0;
    cfg.audio.interface_ip = "127.0.0.1";
    cfg.audio.controller = url;
    CHECK(cfg.token.size() == 64);
    CHECK(iot::to_json(cfg).dump().find(cfg.token) == std::string::npos);
    write_configuration(file, iot::to_json(cfg, true));
    auto host = std::make_unique<iot::Host>(file, AudioBackend::TestNull);
    rejects([&] { iot::Host duplicate(file, AudioBackend::TestNull); });
    iot::Client client("127.0.0.1", host->port(), cfg.token, cfg.audio.id);
    httplib::Client unauthorized("127.0.0.1", host->port());
    auto reply = unauthorized.Get("/v1/config");
    CHECK(reply && reply->status == 403);
    reply =
        unauthorized.Get("/v1/config", httplib::Headers{{"Authorization", "Bearer " + cfg.token},
                                                        {"Origin", "http://127.0.0.1"}});
    CHECK(reply && reply->status == 403);
    CHECK(client.inventory().at("loopbackSupported") == false);
    CHECK(client.configuration().dump().find(cfg.token) == std::string::npos);
    iot::Client wrong_device("127.0.0.1", host->port(), cfg.token, "another-id");
    rejects([&] { wrong_device.status(); });
    eventually([&] {
      const auto d = device(controller, cfg.audio.id);
      return !d.is_null() && d.at("management").at("running") == false;
    });
    cfg.audio.name = "Pi kitchen";
    cfg.audio.endpoints.push_back(
        {"input", "Test capture", "default", EndpointMode::Capture, {{1, 1, 1}, {2, 2, 1}}});
    auto applied = client.configure(1, bridge::to_json(cfg.audio));
    CHECK(applied.at("revision") == 2);
    rejects([&] { client.configure(1, bridge::to_json(cfg.audio)); });
    auto invalid = cfg.audio;
    invalid.endpoints[1].patches[1].network = 1;
    rejects([&] { client.configure(2, bridge::to_json(invalid)); });
    CHECK(client.configuration().at("revision") == 2);
    client.set_running(2, true);
    eventually([&] {
      const auto s = client.status();
      const auto d = device(controller, cfg.audio.id);
      return s.value("audioConnected", false) && !d.is_null() && d.at("streams").size() == 1;
    });
    DeviceConfig peer;
    peer.id = "iot-peer";
    peer.name = "RTP reference";
    peer.host = "127.0.0.1";
    peer.controller = url;
    peer.sender.streams = 1;
    peer.receiver.inputs = 1;
    peer.receiver.port = peer.receiver.monitor_port = 0;
    NetworkDevice reference(peer);
    eventually([&] { return !device(controller, peer.id).is_null(); });
    for (const auto& [tx, rx] :
         {std::pair{peer.id, cfg.audio.id}, std::pair{cfg.audio.id, peer.id}})
      controller.set_route(
          {{"sender", tx}, {"stream", "stream-1"}, {"receiver", rx}, {"input", "input-1"}});
    eventually([&] {
      const auto s = client.status();
      return s.at("rxBytes").get<int>() > 20000 && s.at("txBytes").get<int>() > 20000 &&
             s.at("endpoints")[0].at("peaks")[0].get<float>() > .05f;
    });
    const auto previous_source = device(controller, cfg.audio.id).at("streams")[0].at("ssrc");
    invalid = cfg.audio;
    invalid.rx_channels = 0;
    rejects([&] { client.configure(3, bridge::to_json(invalid)); });
    CHECK(client.status().value("running", false));
    CHECK(device(controller, cfg.audio.id).at("streams")[0].at("ssrc") == previous_source);
    cfg.audio.latency_ms = 100;
    cfg.audio.tx_names = {"Kitchen L", "Kitchen R"};
    client.configure(3, bridge::to_json(cfg.audio));
    eventually([&] {
      const auto s = client.status();
      return s.value("running", false) && s.at("rxBytes").get<int>() > 10000;
    });
    CHECK(read_configuration(file).at("audio").at("latencyMs") == 100);
    // A persistence failure must retain the old revision/settings and restore the previous audio.
    auto blocked = file;
    blocked += ".tmp";
    std::filesystem::create_directory(blocked);
    auto unsaved = cfg.audio;
    unsaved.name = "Must not be saved";
    rejects([&] { client.configure(4, bridge::to_json(unsaved)); });
    CHECK(client.configuration().at("revision") == 4);
    CHECK(read_configuration(file).at("audio").at("name") == "Pi kitchen");
    CHECK(std::filesystem::remove(blocked));
    eventually([&] { return client.status().value("audioConnected", false); });
    client.set_running(4, false);
    eventually([&] { return device(controller, cfg.audio.id).at("streams").empty(); });
    CHECK(!client.status().value("running", true));
    CHECK(client.configuration().at("revision") == 5);
    host.reset();
    host = std::make_unique<iot::Host>(file, AudioBackend::TestNull);
    iot::Client restarted("127.0.0.1", host->port(), cfg.token, cfg.audio.id);
    CHECK(restarted.configuration().at("audio").at("name") == "Pi kitchen");
    CHECK(!restarted.status().value("running", true));
    restarted.set_running(5, true);
    eventually([&] { return restarted.status().value("audioConnected", false); });
    std::cout
        << "PASS IoT authentication, secret redaction, instance ownership, stopped discovery, "
           "revision conflicts, invalid-config preservation, bidirectional RTP, real playback "
           "meters, remote reconfiguration, persisted Stop/Start and process restart\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
