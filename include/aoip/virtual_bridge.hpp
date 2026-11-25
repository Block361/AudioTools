#pragma once
#ifdef _WIN32
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <windows.h>
namespace aoip::vsc {
inline constexpr std::uint32_t bridge_magic = 0x414f4950, bridge_version = 1, max_channels = 32,
                               capacity_frames = 16384;
inline constexpr wchar_t mapping_name[] = L"Local\\OpenAoIP.VirtualSoundcard.Audio.v1";
struct alignas(64) SharedRing {
  alignas(8) volatile LONG64 read = 0, write = 0, dropped = 0, underruns = 0;
  float data[capacity_frames * max_channels];
};
struct SharedAudio {
  std::uint32_t magic, version, size, sample_rate, tx_channels, rx_channels, latency_ms;
  volatile LONG active, owner_pid, asio_pid;
  alignas(8) volatile LONG64 asio_callbacks, network_tx_frames, network_rx_frames;
  SharedRing to_network, from_network;
};
inline std::uint64_t load64(volatile LONG64& value) {
  return static_cast<std::uint64_t>(InterlockedCompareExchange64(&value, 0, 0));
}
std::size_t ring_write(SharedRing&, std::span<const float>, std::size_t channels);
std::size_t ring_read(SharedRing&, std::span<float>, std::size_t channels);
std::size_t ring_available(SharedRing&);
class BridgeMapping {
public:
  explicit BridgeMapping(bool create, const std::wstring& name = mapping_name);
  ~BridgeMapping();
  BridgeMapping(const BridgeMapping&) = delete;
  BridgeMapping& operator=(const BridgeMapping&) = delete;
  SharedAudio* get() const;
  void configure(int tx_channels, int rx_channels, int rate, int active_state = 1);
  void set_preferred_buffer(int frames);
  int preferred_buffer() const;
  int active_buffer() const;
  void publish_active_buffer(int frames);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace aoip::vsc
#endif
