#include "aoip/audio.hpp"
#include "aoip/network.hpp"
#include "aoip/system.hpp"
#include "httplib.h"
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
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
template <class F> void eventually(F f, int timeout_ms = 8000) {
  const auto deadline = monotonic_ms() + timeout_ms;
  do {
    if (f())
      return;
    std::this_thread::sleep_for(25ms);
  } while (monotonic_ms() < deadline);
  throw std::runtime_error("Timed out waiting for condition");
}
Packet packet(std::uint16_t sequence, std::uint32_t timestamp = 0) {
  Packet p;
  p.sequence = sequence;
  p.timestamp = timestamp;
  p.ssrc = 0x12345678;
  p.pcm = tone_packet(std::uint64_t(sequence) * 240, 440);
  return p;
}
Json device(const Controller& controller, const std::string& id) {
  const auto state = controller.state();
  for (const auto& d : state.at("devices"))
    if (d.at("id") == id)
      return d;
  return nullptr;
}
Json input_stats(const Controller& controller, const std::string& id, int input = 0) {
  const auto d = device(controller, id);
  if (d.is_null())
    return Json::object();
  return d.at("inputs").at(input).at("stats");
}
Json route(const std::string& sender, int stream, const std::string& receiver, int input) {
  return {{"sender", sender},
          {"stream", "stream-" + std::to_string(stream)},
          {"receiver", receiver},
          {"input", "input-" + std::to_string(input)}};
}
Json post(httplib::Client& client, const std::string& path, const Json& body, int code = 200) {
  auto result = client.Post(path, body.dump(), "application/json");
  CHECK(result);
  CHECK(result->status == code);
  return Json::parse(result->body);
}
double frequency(const Bytes& pcm) {
  const std::size_t frames = std::min<std::size_t>(pcm.size() / 4, 24000),
                    start = pcm.size() / 4 - frames;
  int crossings = 0, previous = 0;
  for (std::size_t i = start; i < pcm.size() / 4; ++i) {
    const unsigned raw = (unsigned(pcm[i * 4]) << 8) | pcm[i * 4 + 1];
    const int value = raw >= 32768 ? int(raw) - 65536 : int(raw);
    if (previous < 0 && value >= 0)
      ++crossings;
    previous = value;
  }
  return crossings * 48000.0 / frames;
}
void protocol_test() {
  const auto p = packet(65535, 0xffffff00);
  const auto bytes = encode_packet(p);
  const auto decoded = decode_packet(bytes);
  CHECK(bytes.size() == 972);
  CHECK(bytes[0] == 0x80 && bytes[1] == 96);
  CHECK(bytes[2] == 255 && bytes[3] == 255);
  CHECK(decoded.sequence == p.sequence && decoded.timestamp == p.timestamp &&
        decoded.ssrc == p.ssrc && decoded.pcm == p.pcm);
  CHECK(sequence_distance(0, 65535) == 1);
  CHECK(sequence_distance(65535, 0) == -1);
  rejects([&] { decode_packet(std::span(bytes).first(100)); });
  auto bad = bytes;
  bad[0] = 0x90;
  rejects([&] { decode_packet(bad); });
  bad = bytes;
  bad[1] = 97;
  rejects([&] { decode_packet(bad); });
}
void audio_test() {
  const auto pcm = tone_packet(0, 440);
  const auto l = levels(pcm);
  CHECK(l.rms > 0.13 && l.rms < 0.15);
  CHECK(l.peak < 0.2);
  CHECK(levels(Pcm{}).rms == 0);
  const Bytes source(pcm.begin(), pcm.end());
  const auto wav = encode_wav(source);
  CHECK(decode_wav(wav) == source);
  CHECK(wav[44] == pcm[1] && wav[45] == pcm[0]);
  CHECK(wav_packet(source, 240) == pcm);
  rejects([] { decode_wav(Bytes(44)); });
  auto bad = wav;
  bad[24] = 0;
  rejects([&] { decode_wav(bad); });
  rejects([&] { decode_wav(std::span(wav).first(wav.size() - 1)); });
  AudioRing ring;
  for (int i = 0; i < 2010; ++i)
    ring.push(pcm);
  CHECK(ring.seconds() == 10);
  CHECK(ring.snapshot().size() == 1920000);
  ring.reset();
  CHECK(ring.snapshot().empty());
}
void jitter_test() {
  JitterBuffer jitter;
  const auto a = packet(65534, 0xffffff00), b = packet(65535, 0xfffffff0), c = packet(0, 224);
  jitter.push(a, 0);
  jitter.push(c, 10);
  jitter.push(b, 11);
  jitter.push(b, 12);
  CHECK(!jitter.take(59));
  CHECK(jitter.take(60).value() == a.pcm);
  CHECK(jitter.take(65).value() == b.pcm);
  CHECK(jitter.take(70).value() == c.pcm);
  CHECK(jitter.stats.reordered == 1 && jitter.stats.duplicates == 1);
  CHECK(jitter.take(75).value() == Pcm{});
  CHECK(jitter.stats.lost == 1);
  jitter.push(a, 80);
  CHECK(jitter.stats.late == 1);
  CHECK(!jitter.take(1100));
  CHECK(jitter.stats.resets == 1);
  jitter.push(packet(400, 0), 1200);
  CHECK(!jitter.take(1259));
  CHECK(jitter.take(1260));
  jitter.push(packet(900, 120000), 1270);
  CHECK(jitter.queued() == 1);
  CHECK(jitter.stats.resets == 2);
}
void udp_test() {
  UdpSocket sender(0, "127.0.0.1"), receiver(0, "127.0.0.1");
  const auto encoded = encode_packet(packet(17));
  CHECK(!receiver.receive());
  CHECK(sender.send(encoded, "127.0.0.1", receiver.port()));
  eventually([&] {
    const auto bytes = receiver.receive();
    if (!bytes)
      return false;
    CHECK(*bytes == encoded);
    return true;
  });
  Bytes malformed(5000, 0xff);
  CHECK(sender.send(malformed, "127.0.0.1", receiver.port()));
  eventually([&] {
    const auto bytes = receiver.receive();
    if (!bytes)
      return false;
    CHECK(bytes->size() == 5000);
    rejects([&] { decode_packet(*bytes); });
    return true;
  });
}
void integration_test() {
  const auto state_file =
      (std::filesystem::current_path() / ("routes-test-" + random_id() + ".json")).string();
  struct Cleanup {
    std::string file;
    ~Cleanup() {
      std::error_code error;
      std::filesystem::remove(file, error);
      std::filesystem::remove(file + ".tmp", error);
    }
  } cleanup{state_file};
  ControllerConfig cc;
  cc.port = 0;
  cc.state_file = state_file;
  auto controller = std::make_unique<Controller>(cc);
  const int port = controller->port();
  const std::string url = "http://127.0.0.1:" + std::to_string(port);
  SenderConfig sc;
  sc.id = "tx";
  sc.controller = url;
  sc.host = "127.0.0.1";
  sc.streams = 2;
  auto sender = std::make_unique<Sender>(sc);
  ReceiverConfig rc;
  rc.id = "rx";
  rc.controller = url;
  rc.port = 0;
  rc.host = "127.0.0.1";
  Receiver receiver(rc);
  rc.id = "rx2";
  Receiver receiver2(rc);
  eventually([&] { return controller->state().at("devices").size() == 3; });
  const auto initial_events = controller->state().at("events");
  CHECK(initial_events.size() == 3);
  for (const auto& event : initial_events) {
    CHECK(event.is_object());
    CHECK(event.at("at").is_number_integer());
    CHECK(event.at("message").is_string());
  }
  httplib::Client http("127.0.0.1", port);
  http.set_read_timeout(3, 0);
  http.set_connection_timeout(1, 0);
  post(http, "/api/routes", route("tx", 1, "rx", 1));
  post(http, "/api/routes", route("tx", 2, "rx", 2));
  post(http, "/api/routes", route("tx", 1, "rx2", 1));
  eventually([&] {
    const auto a = input_stats(*controller, "rx", 0), b = input_stats(*controller, "rx", 1),
               c = input_stats(*controller, "rx2", 0);
    return a.value("capturedSeconds", 0.0) > 0.5 && b.value("capturedSeconds", 0.0) > 0.5 &&
           c.value("capturedSeconds", 0.0) > 0.5;
  });
  const auto get_audio = [&](const std::string& path) {
    auto result = http.Get(path);
    CHECK(result);
    CHECK(result->status == 200);
    return decode_wav(
        std::span(reinterpret_cast<const std::uint8_t*>(result->body.data()), result->body.size()));
  };
  CHECK(std::abs(frequency(get_audio("/api/audio/rx/input-1.wav")) - 440) < 20);
  CHECK(std::abs(frequency(get_audio("/api/audio/rx/input-2.wav")) - 550) < 20);
  CHECK(std::abs(frequency(get_audio("/api/audio/rx2/input-1.wav")) - 440) < 20);
  post(http, "/api/routes", route("missing", 1, "rx", 1), 400);
  CHECK(!controller->state().at("localAudioActive").get<bool>());
  post(http, "/api/hardware", {{"capture", ""}, {"playback", ""}}, 400);
  post(http, "/api/hardware",
       {{"capture", "default"},
        {"streams", 1},
        {"inputs", 1},
        {"volume", 0},
        {"latency", 60},
        {"captureMap", Json::array({33, 33})}},
       400);
  post(http, "/api/hardware",
       {{"capture", "default"}, {"streams", 0}, {"inputs", 1}, {"volume", 0}, {"latency", 60}},
       400);
  auto cross = http.Post("/api/routes", httplib::Headers{{"Origin", "https://example.com"}},
                         route("tx", 1, "rx", 1).dump(), "application/json");
  CHECK(cross && cross->status == 403);
  auto invalid = http.Post("/api/routes", "{broken", "application/json");
  CHECK(invalid && invalid->status == 400);
  auto duplicate = device(*controller, "tx");
  duplicate["instance"] = "different-instance";
  post(http, "/api/heartbeat", duplicate, 409);
  // Oversized/malformed and wrong-SSRC packets must not kill the receiver.
  UdpSocket injector(0, "127.0.0.1");
  const auto input_port = device(*controller, "rx").at("inputs").at(0).at("port").get<int>();
  CHECK(injector.send(Bytes(5000, 0xff), "127.0.0.1", input_port));
  CHECK(injector.send(encode_packet(packet(99)), "127.0.0.1", input_port));
  eventually([&] {
    const auto s = input_stats(*controller, "rx");
    return s.value("malformed", 0) > 0 && s.value("foreign", 0) > 0 && s.value("rms", 0.0) > 0.1;
  });
  // Reroute and confirm the actual received waveform changed.
  post(http, "/api/routes", route("tx", 2, "rx", 1));
  eventually([&] {
    auto s = input_stats(*controller, "rx");
    return s.value("source", std::string{}).find("550") != std::string::npos &&
           s.value("capturedSeconds", 0.0) > 0.5;
  });
  CHECK(std::abs(frequency(get_audio("/api/audio/rx/input-1.wav")) - 550) < 20);
  // A duplicate sender ID is released after the offline timeout, then routes reconnect.
  sender.reset();
  eventually([&] { return !device(*controller, "tx").at("online").get<bool>(); });
  CHECK(!controller->state().at("routes").at(0).at("active").get<bool>());
  sender = std::make_unique<Sender>(sc);
  eventually([&] {
    return input_stats(*controller, "rx").value("capturedSeconds", 0.0) > 0.5 &&
           device(*controller, "tx").at("online").get<bool>();
  });
  // Stop control service: endpoints keep their last applied audio routes.
  const auto before = input_stats(*controller, "rx").value("received", 0ULL);
  controller.reset();
  std::this_thread::sleep_for(200ms);
  cc.port = port;
  controller = std::make_unique<Controller>(cc);
  CHECK(!controller->fresh());
  CHECK(controller->state().at("routes").size() == 3);
  eventually([&] { return input_stats(*controller, "rx").value("received", 0ULL) > before + 10; });
  // Disconnect clears the receiver capture and stops its stream assignment.
  auto removed = http.Delete("/api/routes", Json{{"receiver", "rx"}, {"input", "input-1"}}.dump(),
                             "application/json");
  CHECK(removed && removed->status == 200);
  eventually(
      [&] { return input_stats(*controller, "rx").value("state", std::string{}) == "unrouted"; });
  auto no_audio = http.Get("/api/audio/rx/input-1.wav");
  CHECK(no_audio && no_audio->status == 409);
  CHECK(controller->state().at("routes").size() == 2);
  // Exercise a real WAV source through the complete sender/receiver pipeline.
  const auto wav_file = state_file + ".wav";
  Cleanup wav_cleanup{wav_file};
  Bytes wav_pcm;
  for (int i = 0; i < 200; ++i) {
    const auto block = tone_packet(static_cast<std::uint64_t>(i) * frames_per_packet, 880);
    wav_pcm.insert(wav_pcm.end(), block.begin(), block.end());
  }
  const auto wav = encode_wav(wav_pcm);
  {
    std::ofstream file(wav_file, std::ios::binary);
    file.write(reinterpret_cast<const char*>(wav.data()), static_cast<std::streamsize>(wav.size()));
    CHECK(file.good());
  }
  sc.id = "wav-tx";
  sc.streams = 1;
  sc.wav = {wav_file};
  Sender wav_sender(sc);
  eventually([&] { return !device(*controller, "wav-tx").is_null(); });
  post(http, "/api/routes", route("wav-tx", 1, "rx", 1));
  eventually([&] { return input_stats(*controller, "rx").value("capturedSeconds", 0.0) > 0.5; });
  CHECK(std::abs(frequency(get_audio("/api/audio/rx/input-1.wav")) - 880) < 20);
}
int main() {
  NetworkRuntime runtime;
  int failed = 0;
  const std::vector<std::pair<std::string, std::function<void()>>> tests = {
      {"RTP packet encoding and validation", protocol_test},
      {"PCM, WAV parsing and bounded capture", audio_test},
      {"Jitter buffering, loss, reordering and wraparound", jitter_test},
      {"Actual UDP delivery and malformed datagrams", udp_test},
      {"Multi-stream routing, fan-out, capture and restart recovery", integration_test}};
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << std::endl;
    } catch (const std::exception& e) {
      ++failed;
      std::cerr << "FAIL " << name << ": " << e.what() << std::endl;
    }
  }
  return failed ? 1 : 0;
}
