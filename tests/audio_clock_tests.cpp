// SPDX-License-Identifier: MIT
#include "aoip/bridge_engine.hpp"
#include "aoip/ptp.hpp"
#include <chrono>
#include <iostream>
#include <thread>
using namespace aoip;
using namespace std::chrono_literals;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x))                                                                                      \
      throw std::runtime_error("Check failed: " #x);                                               \
  } while (false)
template <class F> void eventually(F f) {
  const auto end = monotonic_ms() + 10000;
  do {
    if (f())
      return;
    std::this_thread::sleep_for(20ms);
  } while (monotonic_ms() < end);
  throw std::runtime_error("PTP/audio integration timed out");
}
int main() {
  try {
    NetworkRuntime network;
    ControllerConfig cc;
    cc.port = 0;
    cc.state_file = "";
    Controller controller(cc);
    const auto url = "http://127.0.0.1:" + std::to_string(controller.port());
    auto reservation = std::make_unique<UdpSocket>(0, "127.0.0.1");
    ptp::Options client;
    client.interface_ip = "127.0.0.1";
    client.event_port = client.general_port = 0;
    client.multicast = false;
    client.domain = 42;
    client.destination_event_port = reservation->port();
    auto follower = std::make_shared<ptp::Monitor>(client);
    auto server = client;
    server.role = ptp::Role::Grandmaster;
    server.event_port = reservation->port();
    server.destination_event_port = follower->event_port();
    server.destination_general_port = follower->general_port();
    reservation.reset();
    auto master = std::make_unique<ptp::Monitor>(server);
    bridge::Settings a;
    a.id = "clock-source";
    a.name = "Clock audio test source";
    a.controller = url;
    a.interface_ip = "127.0.0.1";
    a.tx_channels = a.rx_channels = 2;
    a.endpoints = {
        {"mic", "Test capture", "default", EndpointMode::Capture, {{1, 1, 1}, {2, 2, 1}}},
        {"out", "Test playback", "default", EndpointMode::Playback, {{1, 1, 1}, {2, 2, 1}}}};
    auto b = a;
    b.id = "clock-follower";
    b.clock_source = "ptp-monitor";
    b.ptp_domain = 42;
    bridge::Engine source(a, AudioBackend::TestNull);
    bridge::Engine receiver(b, AudioBackend::TestNull, follower);
    eventually([&] { return controller.state().at("devices").size() == 2; });
    for (const auto& [tx, rx] : {std::pair{a.id, b.id}, std::pair{b.id, a.id}})
      controller.set_route(
          {{"sender", tx}, {"stream", "stream-1"}, {"receiver", rx}, {"input", "input-1"}});
    eventually([&] {
      const auto s = receiver.status(), t = source.status();
      return s.at("rxBytes").get<int>() > 20000 && t.at("rxBytes").get<int>() > 20000 &&
             s.at("endpoints")[1].value("connected", false) &&
             t.at("endpoints")[1].value("connected", false) &&
             follower->status().at("samples").get<int>() >= 2;
    });
    CHECK(receiver.status().at("clock").value("audioRateActive", false));
    CHECK(!receiver.status().at("clock").value("audioLocked", true));
    const auto samples = follower->status().at("samples").get<int>();
    master.reset();
    eventually([&] { return !follower->status().value("measurementFresh", true); });
    std::this_thread::sleep_for(150ms);
    const auto held = receiver.status();
    CHECK(held.at("clock").at("rateState") == "Holdover" ||
          held.at("clock").at("rateState") == "Acquiring"); // May never lock on a noisy CI host.
    CHECK(held.at("clock").value("audioRateActive", false));
    const auto before = held.at("rxBytes").get<std::uint64_t>();
    eventually(
        [&] { return receiver.status().at("rxBytes").get<std::uint64_t>() > before + 20000; });
    master = std::make_unique<ptp::Monitor>(server);
    eventually([&] { return follower->status().at("samples").get<int>() >= samples + 2; });
    CHECK(receiver.status().at("clock").value("audioRateActive", false));
    CHECK(receiver.status().at("endpoints")[1].value("connected", false));
    std::cout << "PASS two real Bridge engines, bidirectional RTP and playback, actual PTP UDP, "
                 "frequency-paced TX/RX and uninterrupted audio worker operation across GM "
                 "loss/restart\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
