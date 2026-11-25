#include "aoip/discovery.hpp"
#include "aoip/network.hpp"
#include "aoip/virtual_bridge.hpp"
#include "aoip/vsc_engine.hpp"
#include "httplib.h"
#include "matrix_model.hpp"
#include <iostream>
#include <thread>
using namespace aoip;
void check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
template <class F> void rejects(F function) {
  bool rejected = false;
  try {
    function();
  } catch (const std::exception&) {
    rejected = true;
  }
  check(rejected, "Expected rejection");
}
void discovery_test() {
  int port = 0;
  {
    UdpSocket reserve(0, "127.0.0.1");
    port = reserve.port();
  }
  DiscoveryServer server(43210, "127.0.0.1", port);
  UdpSocket sender(0, "127.0.0.1");
  for (const std::string text : {"[]", "null", "{\"protocol\":3}", "garbage"})
    sender.send(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()),
                "127.0.0.1", port);
  const auto found = discover_controllers("127.0.0.1", 150, port);
  check(found.size() == 1 && found[0].port == 43210 && found[0].url == "http://127.0.0.1:43210",
        "UDP discovery failed after malformed datagrams");
}
Json heartbeat(httplib::Client& http, Json body) {
  const auto r = http.Post("/api/heartbeat", body.dump(), "application/json");
  check(r && r->status == 200, "Heartbeat failed");
  return Json::parse(r->body);
}
void routing_and_matrix_test() {
  ControllerConfig c;
  c.port = 0;
  c.state_file = "";
  Controller controller(c);
  httplib::Client http("127.0.0.1", controller.port());
  auto state = desktop::preview_network();
  for (auto d : state.at("devices")) {
    d["instance"] = "test-instance";
    d["monitorPort"] = 19001;
    for (auto& s : d["streams"])
      s["ssrc"] = 123;
    heartbeat(http, d);
  }
  auto r = Json{{"sender", "preview-0"},
                {"stream", "stream-1"},
                {"receiver", "preview-1"},
                {"input", "input-1"}};
  controller.set_route(r);
  r["senderChannel"] = 2;
  r["receiverChannel"] = 1;
  controller.set_route(r);
  check(controller.state().at("routes").size() == 2, "Changing a side destroyed its stereo peer");
  controller.remove_route("preview-1", "input-1", 1);
  check(controller.state().at("routes").size() == 1 &&
            controller.state().at("routes")[0]["receiverChannel"] == 2,
        "Removing a side destroyed its peer");
  auto odd = state.at("devices")[2];
  odd["instance"] = "test-instance";
  odd["monitorPort"] = 19002;
  for (auto& s : odd["streams"])
    s["ssrc"] = 456;
  odd["streams"][1]["channels"] = 1;
  odd["inputs"][1]["channels"] = 1;
  heartbeat(http, odd);
  r["sender"] = "preview-2";
  r["stream"] = "stream-2";
  r["senderChannel"] = 2;
  rejects([&] { controller.set_route(r); });
  r["senderChannel"] = 1;
  controller.set_route(r);
  odd["format"]["sampleRate"] = 96000;
  heartbeat(http, odd);
  rejects([&] { controller.set_route(r); });
  auto plan = heartbeat(http, odd);
  check(plan.at("sends").empty(), "Format-incompatible saved route remained active");
  desktop::MatrixModel model;
  model.update(controller.state());
  check(model.transmitters().size() == 14 && model.receivers().size() == 14,
        "Odd channel displayed as two physical channels");
  model.expand_all(false);
  check(model.transmitters().size() == 3, "Collapse failed");
  model.update(controller.state());
  check(model.transmitters().size() == 3, "Refresh discarded collapse preference");
  model.expand_all(true);
  check(model.receivers().size() == 14, "Expand failed");
}
void bridge_ownership_test() {
  const auto name = L"Local\\OpenAoIP.Test.ownership." + std::to_wstring(GetCurrentProcessId());
  vsc::Settings s;
  s.bridge_mapping = name;
  s.interface_ip = "127.0.0.1";
  s.controller = "http://127.0.0.1:1";
  s.tx_channels = 3;
  s.rx_channels = 5;
  {
    vsc::Engine engine(s);
    rejects([&] { vsc::Engine other(s); });
    check(engine.status().at("running"), "Engine failed");
  }
  {
    vsc::Engine restarted(s);
    check(restarted.status().at("running"), "Clean restart failed");
  }
  vsc::BridgeMapping holder(true, name);
  holder.get()->owner_pid = 0x7ffffffe;
  holder.get()->active = 1;
  {
    vsc::Engine recovered(s);
    check(recovered.status().at("running"), "Stale process owner prevented recovery");
  }
  s.tx_channels = 0;
  rejects([&] { vsc::Engine invalid(s); });
  holder.get()->asio_pid = GetCurrentProcessId();
  rejects([&] { holder.configure(2, 2, 48000, 2); });
  holder.get()->asio_pid = 0;
  holder.configure(2, 2, 48000, 2);
  check(holder.get()->active == 2, "Windows backend briefly exposed an ASIO-compatible active state");
  holder.get()->active = 0;
}
int main() {
  try {
    NetworkRuntime network;
    discovery_test();
    routing_and_matrix_test();
    bridge_ownership_test();
    std::cout << "PASS discovery, malformed UDP, stereo-side preservation, odd channels, "
                 "sample-rate rejection, matrix expansion and bridge ownership/recovery\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
