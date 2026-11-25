// SPDX-License-Identifier: MIT
#include "aoip/adaptive_buffer.hpp"
#include "aoip/bridge_engine.hpp"
#include "aoip/engine_host.hpp"
#include "aoip/network.hpp"
#include "httplib.h"
#include "matrix_model.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <thread>
using namespace aoip;
using namespace std::chrono_literals;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x))                                                                                      \
      throw std::runtime_error(std::string("Check failed at ") + std::to_string(__LINE__) + ": " + \
                               #x);                                                                \
  } while (false)
template <class F> void eventually(F f) {
  const auto end = monotonic_ms() + 10000;
  do {
    if (f())
      return;
    std::this_thread::sleep_for(20ms);
  } while (monotonic_ms() < end);
  throw std::runtime_error("Suite condition timed out");
}
template <class F> void rejects(F f) {
  bool ok = false;
  try {
    f();
  } catch (const std::exception&) {
    ok = true;
  }
  CHECK(ok);
}
void adaptive_test() {
  for (double drift : {-.0005, .0005}) {
    AdaptiveBuffer buffer(2, 48000, 20);
    std::vector<float> source(482), output(480);
    double fractional = 0;
    std::uint64_t frame = 0;
    for (int block = 0; block < 36000; ++block) {
      fractional += 240 * (1 + drift);
      const auto n = static_cast<int>(fractional);
      fractional -= n;
      for (int i = 0; i < n; ++i) {
        source[i * 2] = std::sin(float(frame + i) * .05f) * .25f;
        source[i * 2 + 1] = -source[i * 2];
      }
      frame += n;
      buffer.write(std::span(source.data(), n * 2));
      buffer.read(output);
      for (int i = 0; i < 240; ++i) {
        CHECK(std::isfinite(output[i * 2]));
        CHECK(std::abs(output[i * 2] + output[i * 2 + 1]) < .00001f);
      }
      if (block > 4000) {
        CHECK(buffer.available() < 2400);
        CHECK(std::abs(buffer.drift_ppm()) <= 2000);
      }
    }
    CHECK(buffer.underruns() == 0);
    CHECK(buffer.dropped() == 0);
  }
  AdaptiveBuffer stale(1, 48000, 20);
  std::vector<float> old(24000, .2f), out(240);
  stale.write(old);
  stale.read(out);
  CHECK(stale.available() < 1300);
  CHECK(stale.dropped() > 20000);
  AdaptiveBuffer concurrent(2, 48000, 5);
  std::atomic<bool> done{false};
  std::thread producer([&] {
    std::vector<float> data(480, .3f);
    for (int n = 0; n < 10000; ++n)
      concurrent.write(data);
    done = true;
  });
  while (!done) {
    std::vector<float> block(480);
    concurrent.read(block);
    for (float v : block)
      CHECK(std::isfinite(v) && v >= 0 && v <= .301f);
  }
  producer.join();
}
void conversion_test() {
  std::vector<float> samples(frames_per_packet * 3, .25f), restored(samples.size());
  std::array<Pcm, 2> pcm{};
  samples[0] = std::numeric_limits<float>::quiet_NaN();
  samples[1] = 2;
  float_to_pcm(samples, pcm, 3);
  pcm_to_float(pcm, restored, 3);
  CHECK(restored[0] == 0);
  CHECK(restored[1] > .999f);
  CHECK(std::abs(restored[2] - .25f) < .0001f);
  for (int f = 0; f < frames_per_packet; ++f)
    CHECK(pcm[1][f * 4 + 2] == 0 && pcm[1][f * 4 + 3] == 0);
}
Json device(const Controller& c, const std::string& id) {
  const auto snapshot = c.state();
  for (const auto& d : snapshot.at("devices"))
    if (d.at("id") == id)
      return d;
  return nullptr;
}
void suite_test(const std::filesystem::path& root) {
  ControllerConfig cc;
  cc.port = 0;
  cc.state_file = (root / L"routes.json").string();
  Controller controller(cc);
  const auto url = "http://127.0.0.1:" + std::to_string(controller.port());
  engine::Options options;
  options.endpoint_file = root / L"engine.json";
  options.backend = AudioBackend::TestNull;
  options.idle_seconds = 0;
  engine::Host host(options);
  rejects([&] { engine::Host duplicate(options); });
  const auto endpoint = read_configuration(options.endpoint_file);
  CHECK(endpoint.at("token").get<std::string>().size() == 64);
  httplib::Client unauthorized("127.0.0.1", host.port());
  auto reply = unauthorized.Get("/v1/status");
  CHECK(reply && reply->status == 403);
  reply =
      unauthorized.Get("/v1/status", httplib::Headers{{"X-OpenAoIP-Token", endpoint.at("token")},
                                                      {"Origin", "http://localhost"}});
  CHECK(reply && reply->status == 403);
  engine::Client client({}, options.endpoint_file);
  CHECK(!client.status("bridge-test").at("active").get<bool>());
  const auto inventory = client.devices();
  CHECK(!inventory.at("capture").empty());
  DeviceConfig source;
  source.id = "tone-source";
  source.name = "Test RTP source";
  source.controller = url;
  source.host = "127.0.0.1";
  source.sender.streams = 2;
  source.sender.tone = 440;
  source.receiver.inputs = 1;
  source.receiver.port = 0;
  NetworkDevice sender(source);
  bridge::Settings s;
  s.id = "bridge-test";
  s.name = "Suite integration test";
  s.controller = url;
  s.interface_ip = "127.0.0.1";
  s.tx_channels = s.rx_channels = 4;
  s.tx_names = {"Mic L", "Mic R", "Spare L", "Spare R"};
  s.endpoints = {{"mic", "Test input", "default", EndpointMode::Capture, {{1, 1, 1}, {2, 2, 1}}},
                 {"out", "Test output", "default", EndpointMode::Playback, {{1, 3, 1}, {2, 4, 1}}}};
  CHECK(bridge::to_json(bridge::from_json(bridge::to_json(s))) == bridge::to_json(s));
  client.start("bridge", bridge::to_json(s));
  rejects([&] { client.start("bridge", bridge::to_json(s)); });
  eventually([&] {
    return client.status(s.id).value("audioConnected", false) &&
           !device(controller, s.id).is_null() && !device(controller, source.id).is_null();
  });
  controller.set_route(
      {{"sender", source.id}, {"stream", "stream-1"}, {"receiver", s.id}, {"input", "input-2"}});
  controller.set_route(
      {{"sender", s.id}, {"stream", "stream-1"}, {"receiver", source.id}, {"input", "input-1"}});
  eventually([&] {
    const auto status = client.status(s.id);
    return status.value("rxBytes", 0ULL) > 20000 && status.value("txBytes", 0ULL) > 20000 &&
           status.at("endpoints")[1].value("connected", false) &&
           status.at("endpoints")[1].value("peaks", Json::array()).size() > 0 &&
           status.at("endpoints")[1].at("peaks")[0].get<float>() > .05f;
  });
  CHECK(client.status(s.id).at("clock").at("locked") == false);
  controller.set_device_name(s.id, "My bridge");
  controller.set_channel_name(s.id, "streams", "stream-1", 1, "Vocal");
  const auto preset = controller.preset();
  CHECK(device(controller, s.id).at("streams")[0].at("labels")[0] == "Vocal");
  desktop::MatrixModel model;
  model.update(controller.state());
  model.tx_filter = "vocal";
  model.expand_all(false);
  CHECK(model.transmitters().size() == 2);
  CHECK(model.transmitters()[1].number == 1);
  auto bad = preset;
  bad["routes"].push_back(bad.at("routes")[0]);
  rejects([&] { controller.restore_preset(bad); });
  CHECK(controller.preset() == preset);
  controller.remove_route(s.id, "input-2", 1);
  controller.restore_preset(preset);
  CHECK(controller.preset() == preset);
  client.stop(s.id);
  CHECK(!client.status(s.id).at("active").get<bool>());
  auto invalid = s;
  invalid.endpoints[0].patches[1].network = 1;
  rejects([&] { client.start("bridge", bridge::to_json(invalid)); });
  invalid = s;
  invalid.clock_source = "ptp";
  rejects([&] { client.start("bridge", bridge::to_json(invalid)); });
  s.endpoints[0].device = "id:device-not-present";
  client.start("bridge", bridge::to_json(s));
  eventually([&] {
    return !client.status(s.id).at("endpoints")[0].value("error", std::string{}).empty();
  });
  CHECK(!client.status(s.id).at("audioConnected").get<bool>());
  client.stop(s.id);
}
int main() {
  try {
    NetworkRuntime network;
    const auto root =
        std::filesystem::temp_directory_path() / ("OpenAoIP-suite-test-" + random_id());
    std::filesystem::create_directories(root);
    adaptive_test();
    conversion_test();
    suite_test(root);
    std::cout << "PASS drift servo (+/-500 ppm), stale/concurrent buffers, odd-channel PCM, "
                 "authenticated engine IPC, instance ownership, WASAPI adapter test backend, "
                 "bidirectional RTP, actual playback meters, missing-device errors, aliases, "
                 "search and atomic presets\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
