#pragma once
#ifdef _WIN32
#include "aoip/system.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
namespace aoip::vsc {
// User-mode transport for the optional, separately signed development driver.
class WdmClient {
public:
  struct Transport {
    virtual ~Transport() = default;
    virtual bool call(std::uint32_t code, std::span<const std::byte> input,
                      std::span<std::byte> output, std::uint32_t& bytes) = 0;
  };
  WdmClient(int tx, int rx, int rate, std::unique_ptr<Transport> transport = {});
  ~WdmClient();
  bool capture(std::span<float> samples);
  void playback(std::span<const float> samples);
  Json status() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
Json wdm_probe();
} // namespace aoip::vsc
#endif
