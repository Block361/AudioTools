// SPDX-License-Identifier: MIT
#include "aoip/adaptive_buffer.hpp"
#include "aoip/audio_clock.hpp"
#include "aoip/ptp.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>
using namespace aoip;
using namespace aoip::ptp;
using namespace std::chrono_literals;
constexpr Time second = 1000000000, base = 1700000000LL * second;
#define CHECK(x)                                                                                   \
  do {                                                                                             \
    if (!(x))                                                                                      \
      throw std::runtime_error(std::string("Check failed at ") + std::to_string(__LINE__) + ": " + \
                               #x);                                                                \
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
  const auto end = monotonic_ms() + 5000;
  do {
    if (f())
      return;
    std::this_thread::sleep_for(10ms);
  } while (monotonic_ms() < end);
  throw std::runtime_error("PTP transport condition timed out");
}
PortIdentity id(std::uint8_t n) {
  PortIdentity p;
  p.clock = {2, 0, 0, 0, 0, 0, 0, n};
  return p;
}
Message announce(PortIdentity source, int domain = 0) {
  Message m;
  m.type = Type::Announce;
  m.source = source;
  m.grandmaster = source.clock;
  m.domain = std::uint8_t(domain);
  m.flags = 0x0c;
  m.utc_offset = 37;
  m.interval = 0;
  return m;
}
Message wire(const Message& m) {
  return decode(encode(m));
}
void codec_test() {
  for (auto type : {Type::Sync, Type::DelayReq, Type::FollowUp, Type::DelayResp, Type::Announce}) {
    Message m = announce(id(2), 127);
    m.type = type;
    m.requesting = id(1);
    m.sequence = 65535;
    m.correction_ns = -1234567;
    m.timestamp_ns = base + 123456789;
    m.steps = 3;
    auto p = encode(m);
    CHECK(p.size() == (type == Type::Announce ? 64 : type == Type::DelayResp ? 54 : 44));
    const auto decoded = decode(p);
    CHECK(decoded.type == type && decoded.source == m.source && decoded.sequence == 65535 &&
          decoded.domain == 127 && decoded.correction_ns == m.correction_ns &&
          decoded.timestamp_ns == m.timestamp_ns);
    if (type == Type::DelayResp)
      CHECK(decoded.requesting == m.requesting);
    if (type == Type::Announce)
      CHECK(decoded.utc_offset == 37 && decoded.grandmaster == m.grandmaster && decoded.steps == 3);
    for (std::size_t n = 0; n < p.size(); ++n)
      rejects([&] { decode(std::span(p.data(), n)); });
  }
  auto valid = encode(announce(id(2)));
  auto bad = valid;
  bad[1] = 1;
  rejects([&] { decode(bad); });
  bad = valid;
  bad[0] |= 0x10;
  rejects([&] { decode(bad); });
  bad = valid;
  bad[0] = 13;
  rejects([&] { decode(bad); });
  bad = valid;
  bad[40] = bad[41] = bad[42] = bad[43] = 255;
  rejects([&] { decode(bad); });
  bad = valid;
  bad[28] = bad[29] = 0;
  rejects([&] { decode(bad); });
  auto tlv = valid;
  tlv.insert(tlv.end(), {0, 8, 0, 2, 0, 0});
  tlv[3] = std::uint8_t(tlv.size());
  CHECK(decode(tlv).type == Type::Announce);
  tlv[67] = 3;
  rejects([&] { decode(tlv); });
  // Inspect fixed wire offsets independently from our round-trip encoder.
  const Bytes raw = {0, 2, 0, 44, 0, 0, 2, 0, 0,    0,    0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 2, 0,
                     0, 0, 0, 0,  0, 2, 0, 1, 0x12, 0x34, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 3};
  const auto decoded = decode(raw);
  CHECK(decoded.timestamp_ns == 2 * second + 3 && decoded.sequence == 0x1234 &&
        decoded.correction_ns == 1 && (decoded.flags & 0x0200));
}
void exchange(Tracker& tracker, PortIdentity master, Time at, Time offset, Time delay, int seq,
              bool two_step, bool reordered = false) {
  tracker.receive(wire(announce(master)), "127.0.0.1", at - 1000000);
  Message sync;
  sync.source = master;
  sync.sequence = std::uint16_t(seq);
  sync.flags = two_step ? 0x0200 : 0;
  sync.correction_ns = -500;
  sync.timestamp_ns = at - delay + offset - (two_step ? 1000 : -500);
  Message follow = sync;
  follow.type = Type::FollowUp;
  follow.flags = 0;
  follow.correction_ns = 1500;
  if (reordered)
    tracker.receive(wire(follow), "127.0.0.1", at - 500000);
  auto initial = sync;
  if (two_step)
    initial.timestamp_ns = 0;
  tracker.receive(wire(initial), "127.0.0.1", at);
  if (two_step && !reordered)
    tracker.receive(wire(follow), "127.0.0.1", at + 10000);
  const auto request = tracker.delay_request(at + 20000000);
  CHECK(request);
  Message response;
  response.type = Type::DelayResp;
  response.source = master;
  response.requesting = request->source;
  response.sequence = request->sequence;
  response.correction_ns = 4000;
  response.timestamp_ns = at + 20000000 + delay + offset + response.correction_ns;
  const auto before = tracker.status(at).at("samples");
  auto foreign = response;
  foreign.requesting = id(99);
  tracker.receive(wire(foreign), "127.0.0.1", at + 21000000);
  CHECK(tracker.status(at).at("samples") == before);
  tracker.receive(wire(response), "127.0.0.1", at + 21000000);
  const auto after = tracker.status(at + 21000000);
  CHECK(after.at("samples").get<int>() == before.get<int>() + 1);
  CHECK(std::abs(after.at("rawOffsetNs").get<Time>() - offset) <= 1);
  CHECK(std::abs(after.at("pathDelayNs").get<Time>() - delay) <= 1);
  tracker.receive(wire(response), "127.0.0.1", at + 22000000);
  CHECK(tracker.status(at + 22000000).at("samples") == after.at("samples"));
}
void tracker_test() {
  Tracker tracker(id(1), 0);
  CHECK(!tracker.delay_request(base));
  CHECK(tracker.status(base).at("offsetNs").is_null());
  const Time offset = 37 * second - 5000000, delay = 120000;
  for (int i = 0; i < 10; ++i)
    exchange(tracker, id(2), base + i * second, offset, delay, 65530 + i, i % 2 == 0, i % 4 == 0);
  auto s = tracker.status(base + 9 * second + 30000000);
  CHECK(s.at("trackingStable") == true);
  CHECK(s.at("offsetNs") == -5000000);
  CHECK(s.at("locked") == false && s.at("audioLocked") == false && s.at("synchronized") == false);
  CHECK(std::abs(s.at("frequencyPpm").get<double>()) < .001);
  tracker.tick(base + 13 * second);
  s = tracker.status(base + 13 * second);
  CHECK(s.at("trackingStable") == false && s.at("masterAddress").is_null());
  exchange(tracker, id(2), base + 14 * second, offset, delay, 12, true);
  CHECK(tracker.status(base + 14 * second + 30000000).at("windowSamples") == 1);
  auto better = announce(id(3));
  better.priority1 = 10;
  tracker.receive(wire(better), "127.0.0.2", base + 14 * second + 40000000);
  s = tracker.status(base + 14 * second + 40000000);
  CHECK(s.at("grandmasterIdentity") == "0200000000000003" && s.at("windowSamples") == 0);
  CHECK(!tracker.delay_request(base + 14 * second + 50000000));
  auto wrong = better;
  wrong.domain = 1;
  wrong.priority1 = 1;
  wrong.source = id(4);
  tracker.receive(wire(wrong), "127.0.0.4", base + 14 * second + 60000000);
  CHECK(tracker.status(base + 14 * second + 60000000).at("grandmasterIdentity") ==
        "0200000000000003");
  Tracker frequency(id(1), 0);
  for (int i = 0; i < 16; ++i)
    exchange(frequency, id(2), base + i * second, offset + i * 50000, delay, i, true);
  s = frequency.status(base + 15 * second + 30000000);
  CHECK(std::abs(s.at("frequencyPpm").get<double>() - 50.) < .001);
  exchange(frequency, id(2), base + 16 * second, offset + 2 * second, delay, 16, true);
  CHECK(frequency.status(base + 16 * second + 30000000).at("clockSteps") == 1);
  CHECK(frequency.status(base + 16 * second + 30000000).at("windowSamples") == 1);
  Tracker stale(id(1), 0);
  exchange(stale, id(2), base, offset, delay, 1, true);
  stale.receive(wire(announce(id(2))), "127.0.0.1", base + 2 * second);
  stale.tick(base + 3100000000LL);
  CHECK(stale.status(base + 3100000000LL).at("state") == "PTP data stale");
  CHECK(stale.status(base + 3100000000LL).at("measurementFresh") == false);
  Tracker timeout(id(1), 0);
  timeout.receive(wire(announce(id(2))), "127.0.0.1", base);
  Message waiting;
  waiting.source = id(2);
  waiting.timestamp_ns = base + offset - delay;
  timeout.receive(wire(waiting), "127.0.0.1", base);
  CHECK(timeout.delay_request(base + 20000000));
  timeout.tick(base + 2500000000LL);
  CHECK(timeout.status(base + 2500000000LL).at("delayTimeouts") == 1);
  rejects([] { Tracker invalid(id(1), 256); });
}
void transport_test() {
  UdpSocket master(0, "127.0.0.1");
  Options o;
  o.interface_ip = "127.0.0.1";
  o.event_port = o.general_port = 0;
  o.destination_event_port = master.port();
  o.multicast = false;
  Monitor monitor(o);
  auto malformed = Bytes{0, 1, 2};
  master.send(malformed, "127.0.0.1", monitor.general_port());
  const auto a = announce(id(2));
  master.send(encode(a), "127.0.0.1", monitor.general_port());
  eventually([&] { return monitor.status().at("masterAddress") == "127.0.0.1"; });
  Message sync;
  sync.source = id(2);
  sync.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::system_clock::now().time_since_epoch())
                          .count() +
                      37 * second;
  master.send(encode(sync), "127.0.0.1", monitor.event_port());
  std::optional<UdpSocket::Datagram> datagram;
  eventually([&] {
    datagram = master.receive_from();
    return bool(datagram);
  });
  const auto request = decode(datagram->data);
  CHECK(request.type == Type::DelayReq && datagram->received_ns > 0);
  Message response;
  response.type = Type::DelayResp;
  response.source = id(2);
  response.requesting = request.source;
  response.sequence = request.sequence;
  response.timestamp_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::system_clock::now().time_since_epoch())
                              .count() +
                          37 * second;
  master.send(encode(response), "127.0.0.1", monitor.general_port());
  eventually([&] { return monitor.status().at("samples").get<int>() == 1; });
  const auto s = monitor.status();
  CHECK(s.at("malformedPackets").get<int>() == 1 && s.at("audioLocked") == false &&
        s.at("offsetNs").is_number());
  CHECK(s.at("pathDelayNs").get<Time>() < second);
  auto shared1 = shared_monitor(o), shared2 = shared_monitor(o);
  CHECK(shared1 == shared2);
  auto conflicting = o;
  conflicting.domain = 1;
  rejects([&] { shared_monitor(conflicting); });
  std::weak_ptr<Monitor> ownership = shared1;
  shared1.reset();
  shared2.reset();
  CHECK(ownership.expired());
  CHECK(shared_monitor(conflicting)->status().at("domain") == 1);
}
void grandmaster_wire_test() {
  UdpSocket events(0, "127.0.0.1"), generals(0, "127.0.0.1"), requests(0, "127.0.0.1");
  Options o;
  o.interface_ip = "127.0.0.1";
  o.role = Role::Grandmaster;
  o.multicast = false;
  o.event_port = o.general_port = 0;
  o.destination_event_port = events.port();
  o.destination_general_port = generals.port();
  Monitor gm(o);
  std::optional<Message> a, s, f, response;
  eventually([&] {
    while (const auto d = events.receive_from()) {
      const auto m = decode(d->data);
      if (m.type == Type::Sync)
        s = m;
    }
    while (const auto d = generals.receive_from()) {
      const auto m = decode(d->data);
      if (m.type == Type::Announce)
        a = m;
      else if (m.type == Type::FollowUp)
        f = m;
    }
    return a && s && f && s->sequence == f->sequence;
  });
  CHECK(a->clock_class == 248 && a->accuracy == 254 && a->time_source == 160 && a->flags == 0 &&
        a->grandmaster == a->source.clock);
  CHECK(s->source == a->source && s->flags == 0x0200 && s->timestamp_ns == 0 &&
        f->timestamp_ns > base && f->flags == 0);
  Message req;
  req.type = Type::DelayReq;
  req.source = id(77);
  req.sequence = 1234;
  req.timestamp_ns = 42 * second;
  req.correction_ns = 1200;
  auto wrong = req;
  wrong.domain = 1;
  requests.send(encode(wrong), "127.0.0.1", gm.event_port());
  requests.send(encode(req), "127.0.0.1", gm.event_port());
  eventually([&] {
    while (const auto d = generals.receive_from()) {
      const auto m = decode(d->data);
      if (m.type == Type::DelayResp)
        response = m;
    }
    return bool(response);
  });
  CHECK(response->source == a->source && response->requesting == req.source &&
        response->sequence == 1234 && response->timestamp_ns > base &&
        response->correction_ns == 1200);
  CHECK(gm.status().at("delayRequestsReceived") == 1 && gm.status().at("delayResponsesSent") == 1);
  CHECK(gm.status().at("audioLocked") == false && gm.status().at("transmitting") == true);
  const auto peer = announce(id(99));
  requests.send(encode(peer), "127.0.0.1", gm.general_port());
  eventually([&] { return gm.status().at("otherGrandmasters") == 1; });
  CHECK(!gm.status().at("warning").get<std::string>().empty());
}
void audio_servo_test() {
  for (const double drift : {-500., 50., 500.}) {
    Tracker tracker(id(1), 0);
    AudioRateServo servo;
    const Time offset = 37 * second - 1317725100; // Large epoch offset must not jump audio.
    double previous = 0;
    for (int i = 0; i < 70; ++i) {
      exchange(tracker, id(2), base + i * second, offset + Time(i * drift * 1000), 120000, i, true);
      const auto measured = tracker.status(base + i * second + 30000000);
      for (int n = 0; n < 10; ++n) {
        servo.update(measured, i + n / 10.);
        CHECK(std::abs(servo.correction_ppm() - previous) <= 2.00001);
        previous = servo.correction_ppm();
      }
    }
    CHECK(servo.status().at("rateReferenceReady") == true);
    CHECK(std::abs(servo.correction_ppm() - drift) < .01);
    CHECK(servo.status().at("phaseSynchronized") == false);
    const auto interval = disciplined_interval_ms(48000, servo.correction_ppm());
    CHECK(std::abs(240. / interval * 1000. / 48000. - (1 + drift * 1e-6)) < 1e-8);
    auto lost = tracker.status(base + 80 * second);
    for (int i = 0; i < 100; ++i)
      servo.update(lost, 70 + i * .1);
    CHECK(servo.status().at("rateState") == "Holdover");
    CHECK(servo.status().at("rateReferenceReady") == false);
    CHECK(servo.correction_ppm() == previous);
    auto invalid = lost;
    invalid["trackingStable"] = true;
    invalid["measurementFresh"] = true;
    invalid["frequencyPpm"] = 1200.;
    servo.update(invalid, 81);
    CHECK(servo.correction_ppm() == previous);
    // Master replacement reacquires gradually instead of resetting the clock or sample timeline.
    invalid["grandmasterIdentity"] = "replacement";
    invalid["frequencyPpm"] = -drift;
    servo.update(invalid, 82);
    CHECK(servo.status().at("rateReferenceReady") == false);
    for (int i = 1; i < 700; ++i)
      servo.update(invalid, 82 + i * .1);
    CHECK(servo.status().at("rateReferenceReady") == true);
    CHECK(std::abs(servo.correction_ppm() + drift) < .01);
    // Two local clock domains joined by the actual continuous SRC, over 3 simulated minutes.
    AdaptiveBuffer buffer(2, 48000, 20);
    std::vector<float> input(484), output(480);
    double remainder = 0;
    std::uint64_t frame = 0;
    for (int block = 0; block < 36000; ++block) {
      remainder += 240 / (1 + drift * 1e-6);
      const int count = int(remainder);
      remainder -= count;
      for (int f = 0; f < count; ++f) {
        input[f * 2] = .25f * std::sin(float(frame + f) * .04f);
        input[f * 2 + 1] = -input[f * 2];
      }
      frame += count;
      buffer.write(std::span(input.data(), count * 2));
      buffer.read(output);
      for (int f = 0; f < 240; ++f)
        CHECK(std::isfinite(output[f * 2]) && std::abs(output[f * 2] + output[f * 2 + 1]) < 1e-6);
    }
    CHECK(buffer.underruns() == 0 && buffer.dropped() == 0);
  }
}
void grandmaster_recovery_test() {
  auto reservation = std::make_unique<UdpSocket>(0, "127.0.0.1");
  const int master_port = reservation->port();
  Options client;
  client.interface_ip = "127.0.0.1";
  client.domain = 23;
  client.multicast = false;
  client.event_port = client.general_port = 0;
  client.destination_event_port = master_port;
  Monitor follower(client);
  Options server = client;
  server.role = Role::Grandmaster;
  server.event_port = master_port;
  server.destination_event_port = follower.event_port();
  server.destination_general_port = follower.general_port();
  reservation.reset();
  auto gm = std::make_unique<Monitor>(server);
  eventually([&] { return follower.status().at("samples").get<int>() >= 2; });
  const auto before = follower.status();
  CHECK(before.at("measurementFresh") == true && before.at("offsetNs").is_number());
  CHECK(std::abs(before.at("offsetNs").get<Time>()) < 20000000);
  CHECK(before.at("grandmasterIdentity") == gm->status().at("grandmasterIdentity"));
  CHECK(before.at("ptpTimescale") == false && gm->status().at("synchronized") == false);
  gm.reset();
  eventually([&] { return follower.status().at("masterAddress").is_null(); });
  CHECK(follower.status().at("measurementFresh") == false);
  gm = std::make_unique<Monitor>(server);
  const auto replacement = gm->status().at("grandmasterIdentity");
  eventually([&] {
    const auto state = follower.status();
    return state.at("grandmasterIdentity") == replacement &&
           state.at("samples").get<int>() > before.at("samples").get<int>();
  });
  CHECK(follower.status().at("audioLocked") == false);
  auto shared1 = shared_monitor(client);
  auto different = client;
  different.role = Role::Grandmaster;
  rejects([&] { shared_monitor(different); });
}
int main() {
  try {
    NetworkRuntime network;
    codec_test();
    tracker_test();
    audio_servo_test();
    transport_test();
    grandmaster_wire_test();
    grandmaster_recovery_test();
    std::cout << "PASS PTPv2 wire fixtures, signed corrections, one/two-step and reordered Sync, "
                 "E2E delay, UTC offset, 50 ppm tracking, master timeout/change, foreign response "
                 "rejection, software-GM Announce/Sync/FollowUp/DelayResp, live GM/monitor "
                 "exchange and GM restart recovery; audio lock remains false\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
