#include "../drivers/wdm/audio_queue.hpp"
#include "../drivers/wdm/protocol.h"
#include "aoip/network.hpp"
#include "aoip/wdm_client.hpp"
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <thread>
#include <windows.h>
#include <winioctl.h>
using namespace aoip;
void check(bool x, const char* message) {
  if (!x)
    throw std::runtime_error(message);
}
template <class F> void rejects(F f) {
  bool rejected = false;
  try {
    f();
  } catch (const std::exception&) {
    rejected = true;
  }
  check(rejected, "Expected invalid WDM operation to be rejected");
}
struct KernelTransport final : vsc::WdmClient::Transport {
  OPENAOIP_WDM_STATUS state{{2, 3, 5, 48000}, 3, 7, 0, 16};
  std::array<aoip_wdm::Queue, 16> render;
  std::vector<OPENAOIP_WDM_BLOCK> writes;
  bool malformed = false, configured = false;
  bool call(std::uint32_t code, std::span<const std::byte> input, std::span<std::byte> output,
            std::uint32_t& bytes) override {
    bytes = 0;
    if (code == OPENAOIP_IOCTL_STATUS) {
      if (output.size() < sizeof(state))
        return false;
      std::memcpy(output.data(), &state, sizeof(state));
      bytes = sizeof(state);
      return true;
    }
    if (code == OPENAOIP_IOCTL_CONFIGURE) {
      if (input.size() != sizeof(state.config))
        return false;
      std::memcpy(&state.config, input.data(), sizeof(state.config));
      configured = true;
      return true;
    }
    if (input.size() != sizeof(OPENAOIP_WDM_BLOCK))
      return false;
    OPENAOIP_WDM_BLOCK block{};
    std::memcpy(&block, input.data(), sizeof(block));
    if (!aoip_wdm::valid_block(block.version, block.pair, block.frames))
      return false;
    if (code == OPENAOIP_IOCTL_READ_RENDER) {
      if (output.size() < sizeof(block))
        return false;
      block.frames = render[block.pair].pop(block.samples, block.frames, false);
      block.running = (state.render_mask >> block.pair) & 1;
      if (malformed)
        block.frames = 241;
      std::memcpy(output.data(), &block, sizeof(block));
      bytes = sizeof(block);
      return true;
    }
    if (code == OPENAOIP_IOCTL_WRITE_CAPTURE) {
      writes.push_back(block);
      return true;
    }
    return false;
  }
};
void queue_test() {
  auto q = std::make_unique<aoip_wdm::Queue>();
  std::vector<short> source((aoip_wdm::capacity + 4) * 2, 1234), target(source.size(), -1);
  check(q->push(source.data(), aoip_wdm::capacity + 4) == aoip_wdm::capacity && q->dropped == 4,
        "Queue overflow was not bounded");
  check(q->pop(target.data(), aoip_wdm::capacity + 4) == aoip_wdm::capacity && q->underruns == 4,
        "Queue underrun counters incorrect");
  check(target[aoip_wdm::capacity * 2] == 0 && target[0] == 1234, "Queue did not silence underrun");
  for (int n = 0; n < 1000; ++n) {
    std::fill_n(source.data(), 480, static_cast<short>(n));
    check(q->push(source.data(), 240) == 240, "Wrapped queue write failed");
    check(q->pop(target.data(), 240) == 240 && target[0] == n && target[479] == n,
          "Wrapped stereo queue corrupted samples");
  }
  check(!aoip_wdm::valid_config(1, 2, 2, 48000) && !aoip_wdm::valid_config(2, 33, 2, 48000) &&
            !aoip_wdm::valid_config(2, 2, 2, 44100),
        "Invalid configuration accepted");
  check(!aoip_wdm::valid_block(2, 16, 240) && !aoip_wdm::valid_block(2, 0, 241) &&
            !aoip_wdm::valid_block(2, 0, 0),
        "Invalid pair or transfer length accepted");
}
void mapping_test() {
  auto io = std::make_unique<KernelTransport>();
  auto* kernel = io.get();
  std::array<short, 480> first{}, second{};
  for (int i = 0; i < 240; ++i) {
    first[i * 2] = 1000;
    first[i * 2 + 1] = -2000;
    second[i * 2] = 3000;
    second[i * 2 + 1] = 31000;
  }
  for (int i = 0; i < 4; ++i) {
    kernel->render[0].push(first.data(), 240);
    kernel->render[1].push(second.data(), 240);
  }
  vsc::WdmClient client(3, 5, 48000, std::move(io));
  check(kernel->configured, "WDM client did not configure the transport");
  std::vector<float> capture(240 * 3);
  const auto end = monotonic_ms() + 250;
  while (monotonic_ms() < end) {
    if (client.capture(capture) && capture[0] != 0)
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  check(std::abs(capture[0] - 1000 / 32768.f) < 1e-6 &&
            std::abs(capture[1] + 2000 / 32768.f) < 1e-6 &&
            std::abs(capture[2] - 3000 / 32768.f) < 1e-6,
        "Independent stereo endpoint mapping or odd TX channel failed");
  std::vector<float> playback(240 * 5);
  for (int i = 0; i < 240; ++i) {
    playback[i * 5] = .25f;
    playback[i * 5 + 1] = -.5f;
    playback[i * 5 + 2] = std::numeric_limits<float>::quiet_NaN();
    playback[i * 5 + 3] = 2.f;
    playback[i * 5 + 4] = .75f;
  }
  client.playback(playback);
  check(kernel->writes.size() == 3, "RX audio was not split into three Windows endpoints");
  check(kernel->writes[0].samples[0] == 8192 && kernel->writes[0].samples[1] == -16384,
        "PCM conversion failed");
  check(kernel->writes[1].samples[0] == 0 && kernel->writes[1].samples[1] == 32767,
        "Nonfinite or out-of-range audio was not sanitized");
  check(kernel->writes[2].samples[0] == 24575 && kernel->writes[2].samples[1] == 0,
        "Odd RX channel leaked into a Windows channel");
  kernel->malformed = true;
  std::this_thread::sleep_for(std::chrono::milliseconds(6));
  rejects([&] { client.capture(capture); });
  auto outdated = std::make_unique<KernelTransport>();
  outdated->state.config.version = 1;
  rejects([&] { vsc::WdmClient wrong(2, 2, 48000, std::move(outdated)); });
  rejects([] { vsc::WdmClient wrong_rate(2, 2, 44100); });
}
int main() {
  try {
    queue_test();
    mapping_test();
    std::cout << "PASS shared kernel queue, overflow/underrun/wrap, 16-pair IOCTL bounds, odd "
                 "channel mapping, PCM sanitation and version mismatch (in-memory transport; no "
                 "kernel-runtime claim)\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "FAIL " << e.what() << '\n';
    return 1;
  }
}
