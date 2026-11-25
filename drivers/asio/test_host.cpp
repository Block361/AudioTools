// SPDX-License-Identifier: GPL-3.0-only
#include "aoip/network.hpp"
#include "aoip/ptp.hpp"
#include "aoip/vsc_engine.hpp"
#include "asio_api.hpp"
#include "driver_id.hpp"
#include <atomic>
#include <cmath>
#include <iostream>
#include <mutex>
#include <numbers>
#include <thread>
#include <vector>
using namespace aoip;
namespace {
std::array<ASIOBufferInfo, 4> buffers{};
int rate = 48000;
std::uint64_t sent_frames = 0;
std::vector<float> left, right;
std::mutex samples_mutex;
std::atomic<int> switches{0};
void buffer_switch(long index, ASIOBool) {
  auto* l = static_cast<const float*>(buffers[0].buffers[index]);
  auto* r = static_cast<const float*>(buffers[1].buffers[index]);
  {
    std::lock_guard lock(samples_mutex);
    if (left.size() < 500000) {
      left.insert(left.end(), l, l + 256);
      right.insert(right.end(), r, r + 256);
    }
  }
  auto* out_l = static_cast<float*>(buffers[2].buffers[index]);
  auto* out_r = static_cast<float*>(buffers[3].buffers[index]);
  for (int i = 0; i < 256; ++i) {
    out_l[i] =
        static_cast<float>(std::sin(2 * std::numbers::pi * 440 * (sent_frames + i) / rate) * .25);
    out_r[i] =
        static_cast<float>(std::sin(2 * std::numbers::pi * 660 * (sent_frames + i) / rate) * .25);
  }
  sent_frames += 256;
  ++switches;
}
void rate_change(ASIOSampleRate) {}
long message(long, long, void*, double*) {
  return 0;
}
template <class F> void eventually(F f) {
  const auto end = monotonic_ms() + 8000;
  do {
    if (f())
      return;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  } while (monotonic_ms() < end);
  throw std::runtime_error("ASIO/network condition timed out");
}
void check(bool value, const char* text) {
  if (!value)
    throw std::runtime_error(text);
}
double frequency(const std::vector<float>& values, int sr) {
  const auto frames = std::min<std::size_t>(values.size(), sr / 2);
  int crossings = 0;
  for (std::size_t i = values.size() - frames + 1; i < values.size(); ++i)
    if (values[i - 1] < 0 && values[i] >= 0)
      ++crossings;
  return crossings * double(sr) / frames;
}
void run(const wchar_t* dll_path, int sample_rate, std::shared_ptr<ptp::Monitor> clock = {}) {
  rate = sample_rate;
  sent_frames = 0;
  switches = 0;
  left.clear();
  right.clear();
  left.reserve(500000);
  right.reserve(500000);
  const auto name = L"Local\\OpenAoIP.Test." + std::to_wstring(GetCurrentProcessId()) + L"." +
                    std::to_wstring(rate);
  SetEnvironmentVariableW(L"OPENAOIP_VSC_TEST_MAPPING", name.c_str());
  ControllerConfig cc;
  cc.port = 0;
  cc.state_file = "";
  Controller controller(cc);
  vsc::Settings cfg;
  cfg.id = "asio-test";
  cfg.controller = "http://127.0.0.1:" + std::to_string(controller.port());
  cfg.interface_ip = "127.0.0.1";
  cfg.tx_channels = cfg.rx_channels = 2;
  cfg.rate = rate;
  cfg.buffer_frames = 512;
  cfg.bridge_mapping = name;
  if (clock)
    cfg.clock_source = "ptp-grandmaster";
  auto engine = std::make_unique<vsc::Engine>(cfg, clock);
  const auto module = LoadLibraryW(dll_path);
  check(module != nullptr, "ASIO DLL failed to load");
  auto get = reinterpret_cast<HRESULT(WINAPI*)(REFCLSID, REFIID, void**)>(
      GetProcAddress(module, "DllGetClassObject"));
  check(get != nullptr, "Missing COM class factory export");
  IClassFactory* factory = nullptr;
  check(SUCCEEDED(get(CLSID_OpenAoIP_ASIO, IID_IClassFactory, reinterpret_cast<void**>(&factory))),
        "COM factory failed");
  IASIO* driver = nullptr;
  check(SUCCEEDED(factory->CreateInstance(nullptr, CLSID_OpenAoIP_ASIO,
                                          reinterpret_cast<void**>(&driver))),
        "ASIO object failed");
  factory->Release();
  check(driver->init(nullptr) == ASIOTrue, "ASIO init failed");
  long input = 0, output = 0;
  check(driver->getChannels(&input, &output) == ASE_OK && input == 2 && output == 2,
        "ASIO channel enumeration failed");
  check(driver->canSampleRate(rate) == ASE_OK, "ASIO sample rate mismatch");
  long minimum = 0, maximum = 0, preferred = 0, granularity = 0;
  check(driver->getBufferSize(&minimum, &maximum, &preferred, &granularity) == ASE_OK &&
            minimum == 64 && maximum == 1024 && preferred == 512 && granularity == -1,
        "ASIO preferred buffer setting was not shared with the driver");
  for (int i = 0; i < 4; ++i) {
    buffers[i] = {};
    buffers[i].isInput = i < 2 ? ASIOTrue : ASIOFalse;
    buffers[i].channelNum = i % 2;
  }
  ASIOCallbacks cb{};
  cb.bufferSwitch = buffer_switch;
  cb.sampleRateDidChange = rate_change;
  cb.asioMessage = message;
  check(driver->createBuffers(buffers.data(), 4, 256, &cb) == ASE_OK, "ASIO createBuffers failed");
  check(engine->status().at("activeBufferFrames") == 256,
        "ASIO negotiated buffer size was not reported");
  eventually([&] { return controller.state().at("devices").size() == 1; });
  controller.set_route({{"sender", "asio-test"},
                        {"stream", "stream-1"},
                        {"senderChannel", 2},
                        {"receiver", "asio-test"},
                        {"input", "input-1"},
                        {"receiverChannel", 1}});
  controller.set_route({{"sender", "asio-test"},
                        {"stream", "stream-1"},
                        {"senderChannel", 1},
                        {"receiver", "asio-test"},
                        {"input", "input-1"},
                        {"receiverChannel", 2}});
  check(driver->start() == ASE_OK, "ASIO start failed");
  bool passed = false;
  try {
    eventually([&] {
      std::lock_guard lock(samples_mutex);
      if (left.size() < static_cast<std::size_t>(rate))
        return false;
      return std::abs(frequency(left, rate) - 660) < 20 &&
             std::abs(frequency(right, rate) - 440) < 20;
    });
    passed = true;
  } catch (...) {
    driver->stop();
    driver->disposeBuffers();
    driver->Release();
    FreeLibrary(module);
    throw;
  }
  check(driver->stop() == ASE_OK, "ASIO stop failed");
  check(passed && switches > 10, "ASIO callbacks did not advance");
  if (clock) {
    const auto status = engine->status();
    check(status.at("clock").value("audioRateLocked", false), "PTP network rate not applied");
    check(status.at("clockBufferMs") == 20, "PTP SRC adapters not active");
    check(!status.at("clock").value("audioLocked", true), "Phase lock must not be fabricated");
  }
  ASIOSamples pos{};
  ASIOTimeStamp time{};
  check(driver->getSamplePosition(&pos, &time) == ASE_OK && (pos.lo || pos.hi),
        "ASIO position did not advance");
  check(driver->disposeBuffers() == ASE_OK, "ASIO buffer disposal failed");
  check(engine->status().at("activeBufferFrames") == 0,
        "Disposed ASIO buffers remained reported as active");
  driver->Release();
  auto unload = reinterpret_cast<HRESULT(WINAPI*)()>(GetProcAddress(module, "DllCanUnloadNow"));
  check(unload && unload() == S_OK, "ASIO objects leaked");
  FreeLibrary(module);
  engine.reset();
  std::cout << "PASS ASIO host -> virtual audio buffers -> RTP/UDP -> individually swapped receive "
               "channels -> ASIO host at "
            << rate << " Hz\n";
}
} // namespace
int wmain(int argc, wchar_t** argv) {
  try {
    if (argc != 2)
      return 2;
    NetworkRuntime runtime;
    for (int r : {44100, 48000, 96000})
      run(argv[1], r);
    UdpSocket sink(0, "127.0.0.1");
    ptp::Options o;
    o.interface_ip = "127.0.0.1";
    o.role = ptp::Role::Grandmaster;
    o.multicast = false;
    o.event_port = o.general_port = 0;
    o.destination_event_port = o.destination_general_port = sink.port();
    auto clock = std::make_shared<ptp::Monitor>(o);
    for (int r : {44100, 48000, 96000})
      run(argv[1], r, clock);
    SetEnvironmentVariableW(L"OPENAOIP_VSC_TEST_MAPPING", nullptr);
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
