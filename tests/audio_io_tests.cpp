#include "aoip/audio_io.hpp"
#include "aoip/network.hpp"
#include "aoip/system.hpp"
#include <atomic>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace aoip;
using namespace std::chrono_literals;
#define CHECK(expression)                                                                          \
  do {                                                                                             \
    if (!(expression))                                                                             \
      throw std::runtime_error(std::string("Check failed at line ") + std::to_string(__LINE__) +   \
                               ": " + #expression);                                                \
  } while (false)
template <class F> void rejects(F f) {
  bool thrown = false;
  try {
    f();
  } catch (const std::exception&) {
    thrown = true;
  }
  CHECK(thrown);
}
template <class F> void eventually(F f) {
  const auto end = monotonic_ms() + 8000;
  do {
    if (f())
      return;
    std::this_thread::sleep_for(10ms);
  } while (monotonic_ms() < end);
  throw std::runtime_error("Audio callback or hardware transport timed out");
}
void fifo_test() {
  AudioFifo fifo(2, 3);
  std::array<float, 8> input{1, 2, 3, 4, 5, 6, 7, 8};
  CHECK(fifo.write(input) == 3);
  CHECK(fifo.available() == 3);
  std::array<float, 4> first{};
  CHECK(fifo.read(first) == 2);
  CHECK(first[0] == 1 && first[3] == 4);
  CHECK(fifo.write(input) == 2);
  std::array<float, 6> rest{};
  CHECK(fifo.read(rest) == 3);
  CHECK(rest == (std::array<float, 6>{5, 6, 1, 2, 3, 4}));
  CHECK(fifo.available() == 0);
  CHECK(fifo.read(rest) == 0);
  rejects([&] { fifo.write(std::span(input).first(1)); });
  rejects([] { AudioFifo invalid(0, 8); });
  AudioFifo concurrent(2, 64);
  std::atomic<bool> good{true};
  std::thread producer([&] {
    for (int i = 0; i < 100000; ++i) {
      const std::array<float, 2> f{float(i), -float(i)};
      while (concurrent.write(f) == 0)
        std::this_thread::yield();
    }
  });
  std::thread consumer([&] {
    for (int i = 0; i < 100000; ++i) {
      std::array<float, 2> f{};
      while (concurrent.read(f) == 0)
        std::this_thread::yield();
      if (f[0] != float(i) || f[1] != -float(i))
        good = false;
    }
  });
  producer.join();
  consumer.join();
  CHECK(good);
  CHECK(concurrent.available() == 0);
}
void channel_test() {
  CHECK(channel_map({}, 1, 1, true) == (std::vector<int>{1, 1}));
  CHECK(channel_map({}, 2, 4, true) == (std::vector<int>{1, 2, 3, 4}));
  CHECK(channel_map({3, 4}, 1, 4, false) == (std::vector<int>{3, 4}));
  rejects([] { channel_map({}, 2, 2, true); });
  rejects([] { channel_map({1, 1}, 1, 2, false); });
  rejects([] { channel_map({0, 1}, 1, 2, true); });
  rejects([] { channel_map({1}, 1, 2, true); });
  std::vector<float> native(frames_per_packet * 4);
  for (int i = 0; i < frames_per_packet; ++i) {
    native[i * 4] = 0.25f;
    native[i * 4 + 1] = -0.5f;
    native[i * 4 + 2] = 2.0f;
    native[i * 4 + 3] = std::numeric_limits<float>::quiet_NaN();
  }
  std::vector<Pcm> packets(2);
  const std::vector<int> map{1, 2, 3, 4};
  capture_to_packets(native, 4, map, packets);
  CHECK(packets[0][0] == 0x20 && packets[0][1] == 0x00);
  CHECK(packets[0][2] == 0xc0 && packets[0][3] == 0x00);
  CHECK(packets[1][0] == 0x7f && packets[1][1] == 0xff && packets[1][2] == 0);
  std::vector<float> out(native.size());
  packets_to_playback(packets, map, 4, 0.5f, out);
  CHECK(std::abs(out[0] - 0.125f) < 0.0001f && std::abs(out[1] + 0.25f) < 0.0001f);
  CHECK(out[2] < 0.5f && out[3] == 0);
  packets_to_playback(packets, map, 4, 0.0f, out);
  for (float sample : out)
    CHECK(sample == 0);
  std::vector<float> mono(frames_per_packet, 0.4f);
  std::array<Pcm, 1> stereo{};
  capture_to_packets(mono, 1, std::vector<int>{1, 1}, stereo);
  CHECK(stereo[0][0] == stereo[0][2] && stereo[0][1] == stereo[0][3]);
}
void callback_test() {
  const auto devices = audio_devices(AudioBackend::TestNull);
  CHECK(!devices.capture.empty() && !devices.playback.empty());
  CaptureDevice capture("default", 1, {}, AudioBackend::TestNull);
  PlaybackDevice playback("default", 1, {}, 0.5f, AudioBackend::TestNull);
  capture.start();
  playback.start();
  std::array<Pcm, 1> block{};
  eventually([&] {
    if (!capture.read(block))
      return false;
    playback.write(block);
    return capture.status().frames >= 480;
  });
  CHECK(capture.status().state == "running" && playback.status().state == "running");
  eventually([&] { return playback.status().frames > 480; });
  rejects([] { CaptureDevice invalid("not-an-existing-device", 1, {}, AudioBackend::TestNull); });
}
void hardware_transport_test() {
  ControllerConfig cc;
  cc.port = 0;
  cc.state_file = "";
  Controller controller(cc);
  const auto url = "http://127.0.0.1:" + std::to_string(controller.port());
  SenderConfig sc;
  sc.id = "hardware-test-tx";
  sc.streams = 1;
  sc.controller = url;
  sc.host = "127.0.0.1";
  sc.capture_device = "default";
  sc.audio_backend = AudioBackend::TestNull;
  ReceiverConfig rc;
  rc.id = "hardware-test-rx";
  rc.inputs = 1;
  rc.port = 0;
  rc.controller = url;
  rc.host = "127.0.0.1";
  rc.playback_device = "default";
  rc.audio_backend = AudioBackend::TestNull;
  Sender sender(sc);
  Receiver receiver(rc);
  eventually([&] { return controller.state().at("devices").size() == 2; });
  controller.set_route(
      {{"sender", sc.id}, {"stream", "stream-1"}, {"receiver", rc.id}, {"input", "input-1"}});
  eventually([&] {
    const auto state = controller.state();
    for (const auto& d : state.at("devices"))
      if (d.at("id") == rc.id)
        return d.at("inputs").at(0).at("stats").value("capturedSeconds", 0.0) > 0.5 &&
               d.at("audio").value("frames", 0ULL) > 24000;
    return false;
  });
  const auto state = controller.state();
  for (const auto& d : state.at("devices")) {
    CHECK(d.at("audio").at("state") == "running");
    CHECK(d.at("audio").at("channels").get<int>() >= 1);
    CHECK(d.at("audio").at("frames").get<std::uint64_t>() > 24000);
    if (d.at("role") == "sender")
      CHECK(d.at("streams").at(0).at("source").get<std::string>().starts_with("Hardware:"));
  }
}
int main() {
  NetworkRuntime runtime;
  int failed = 0;
  for (const auto& [name, test] : std::vector<std::pair<std::string, std::function<void()>>>{
           {"SPSC audio queue and concurrent frame order", fifo_test},
           {"Physical channel mapping and PCM conversion", channel_test},
           {"Native callback lifecycle on explicit test backend", callback_test},
           {"Capture callback -> RTP/UDP -> playback callback", hardware_transport_test}})
    try {
      test();
      std::cout << "PASS " << name << std::endl;
    } catch (const std::exception& e) {
      ++failed;
      std::cerr << "FAIL " << name << ": " << e.what() << std::endl;
    }
  return failed ? 1 : 0;
}
