#include "aoip/wdm_client.hpp"
#ifdef _WIN32
#include "../drivers/wdm/protocol.h"
#include "aoip/network.hpp"
#include <array>
#include <cmath>
#include <mmdeviceapi.h>
#include <mutex>
#include <propsys.h>
#include <windows.h>
#include <winioctl.h>
namespace aoip::vsc {
namespace {
struct DeviceTransport final : WdmClient::Transport {
  HANDLE handle;
  explicit DeviceTransport(bool write) {
    handle = CreateFileW(L"\\\\.\\OpenAoIPWdm", GENERIC_READ | (write ? GENERIC_WRITE : 0),
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
      throw std::runtime_error("Windows-Audiotreiber fehlt oder ist nicht erreichbar. Zuerst ein "
                               "signiertes OpenAoIP-WDM-Treiberpaket installieren.");
  }
  ~DeviceTransport() override {
    if (handle != INVALID_HANDLE_VALUE)
      CloseHandle(handle);
  }
  bool call(std::uint32_t code, std::span<const std::byte> input, std::span<std::byte> output,
            std::uint32_t& bytes) override {
    DWORD actual = 0;
    const bool result =
        DeviceIoControl(handle, code, const_cast<std::byte*>(input.data()),
                        static_cast<DWORD>(input.size()), output.data(),
                        static_cast<DWORD>(output.size()), &actual, nullptr) != FALSE;
    bytes = actual;
    return result;
  }
};
template <class T> auto writable(T& value) {
  return std::as_writable_bytes(std::span(&value, 1));
}
template <class T> auto readable(const T& value) {
  return std::as_bytes(std::span(&value, 1));
}
OPENAOIP_WDM_STATUS read_status(WdmClient::Transport& transport) {
  OPENAOIP_WDM_STATUS s{};
  std::uint32_t bytes = 0;
  if (!transport.call(OPENAOIP_IOCTL_STATUS, {}, writable(s), bytes) || bytes != sizeof(s) ||
      s.config.version != OPENAOIP_WDM_VERSION || s.pair_count != OPENAOIP_WDM_PAIRS)
    throw std::runtime_error(
        "WDM-Treiberversion passt nicht zur Anwendung. Treiberpaket aktualisieren.");
  return s;
}
Json json_status(const OPENAOIP_WDM_STATUS& s) {
  return {{"available", true},
          {"experimental", true},
          {"version", s.config.version},
          {"pairCount", s.pair_count},
          {"sampleRate", s.config.sample_rate},
          {"renderMask", s.render_mask},
          {"captureMask", s.capture_mask},
          {"ownerPid", s.owner_pid},
          {"renderFrames", s.render_frames},
          {"captureFrames", s.capture_frames},
          {"renderDropped", s.render_dropped},
          {"captureUnderruns", s.capture_underruns}};
}
} // namespace
struct WdmClient::Impl {
  int tx, rx;
  std::unique_ptr<Transport> transport;
  double deadline = 0;
  unsigned long last_render_mask = 0;
  std::array<double, OPENAOIP_WDM_PAIRS> ready_at{};
  mutable std::mutex mutex;
  Impl(int t, int r, int rate, std::unique_ptr<Transport> io)
      : tx(t), rx(r), transport(std::move(io)) {
    if (t < 1 || t > 32 || r < 1 || r > 32 || rate != OPENAOIP_WDM_RATE)
      throw std::runtime_error("Windows-Audio benoetigt 1-32 Netzwerkkanaele und 48000 Hz. Die "
                               "Windows-Geraete sind feste Stereo-Paare.");
    if (!transport)
      transport = std::make_unique<DeviceTransport>(true);
    read_status(*transport);
    OPENAOIP_WDM_CONFIG c{OPENAOIP_WDM_VERSION, static_cast<ULONG>(t), static_cast<ULONG>(r),
                          static_cast<ULONG>(rate)};
    std::uint32_t bytes = 0;
    if (!transport->call(OPENAOIP_IOCTL_CONFIGURE, readable(c), {}, bytes))
      throw std::runtime_error(
          "Windows-Audiobruecke ist bereits von einer anderen Soundcard-Instanz belegt.");
  }
};
WdmClient::WdmClient(int tx, int rx, int rate, std::unique_ptr<Transport> transport)
    : impl_(std::make_unique<Impl>(tx, rx, rate, std::move(transport))) {}
WdmClient::~WdmClient() = default;
bool WdmClient::capture(std::span<float> samples) {
  auto& d = *impl_;
  std::lock_guard lock(d.mutex);
  if (samples.size() != static_cast<std::size_t>(frames_per_packet * d.tx))
    throw std::runtime_error("Unexpected Windows capture block size");
  const auto now = monotonic_ms();
  if (now < d.deadline)
    return false;
  d.deadline = d.deadline && now - d.deadline < 20 ? d.deadline + 5 : now + 5;
  const auto status = read_status(*d.transport);
  std::fill(samples.begin(), samples.end(), 0.f);
  bool active = false;
  for (int pair = 0; pair < (d.tx + 1) / 2; ++pair) {
    const auto bit = 1u << pair;
    if (!(status.render_mask & bit)) {
      d.ready_at[pair] = 0;
      continue;
    }
    active = true;
    if (!(d.last_render_mask & bit))
      d.ready_at[pair] = now + 20;
    if (now < d.ready_at[pair])
      continue;
    OPENAOIP_WDM_BLOCK request{};
    request.version = OPENAOIP_WDM_VERSION;
    request.pair = pair;
    request.frames = frames_per_packet;
    OPENAOIP_WDM_BLOCK reply{};
    std::uint32_t bytes = 0;
    if (!d.transport->call(OPENAOIP_IOCTL_READ_RENDER, readable(request), writable(reply), bytes) ||
        bytes != sizeof(reply) || reply.version != OPENAOIP_WDM_VERSION ||
        reply.pair != static_cast<ULONG>(pair) || reply.frames > frames_per_packet)
      throw std::runtime_error("Windows render transport returned an invalid block");
    for (unsigned frame = 0; frame < reply.frames; ++frame)
      for (int side = 0; side < 2; ++side) {
        const int channel = pair * 2 + side;
        if (channel < d.tx)
          samples[frame * d.tx + channel] = reply.samples[frame * 2 + side] / 32768.f;
      }
  }
  d.last_render_mask = status.render_mask;
  return active;
}
void WdmClient::playback(std::span<const float> samples) {
  auto& d = *impl_;
  std::lock_guard lock(d.mutex);
  if (samples.size() != static_cast<std::size_t>(frames_per_packet * d.rx))
    throw std::runtime_error("Unexpected Windows playback block size");
  for (int pair = 0; pair < (d.rx + 1) / 2; ++pair) {
    OPENAOIP_WDM_BLOCK block{};
    block.version = OPENAOIP_WDM_VERSION;
    block.pair = pair;
    block.frames = frames_per_packet;
    for (int frame = 0; frame < frames_per_packet; ++frame)
      for (int side = 0; side < 2; ++side) {
        const int channel = pair * 2 + side;
        if (channel >= d.rx)
          continue;
        const auto value = samples[frame * d.rx + channel];
        block.samples[frame * 2 + side] = static_cast<short>(
            std::lround(std::clamp(std::isfinite(value) ? value : 0.f, -1.f, 1.f) * 32767));
      }
    std::uint32_t bytes = 0;
    if (!d.transport->call(OPENAOIP_IOCTL_WRITE_CAPTURE, readable(block), {}, bytes))
      throw std::runtime_error("Windows capture transport write failed");
  }
}
Json WdmClient::status() const {
  auto& d = *impl_;
  std::lock_guard lock(d.mutex);
  try {
    return json_status(read_status(*d.transport));
  } catch (const std::exception& e) {
    return {{"available", false}, {"error", e.what()}};
  }
}
Json wdm_probe() {
  Json result;
  try {
    DeviceTransport transport(false);
    result = json_status(read_status(transport));
  } catch (const std::exception& e) {
    result = {{"available", false}, {"error", e.what()}};
  }
  result["windowsRenderEndpoints"] = 0;
  result["windowsCaptureEndpoints"] = 0;
  result["windowsEndpointsChecked"] = false;
  const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  struct Apartment {
    HRESULT result;
    ~Apartment() {
      if (SUCCEEDED(result))
        CoUninitialize();
    }
  } apartment{initialized};
  IMMDeviceEnumerator* enumerator = nullptr;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&enumerator))))
    return result;
  IMMDeviceCollection* devices = nullptr;
  const auto scanned = enumerator->EnumAudioEndpoints(eAll, DEVICE_STATE_ACTIVE, &devices);
  enumerator->Release();
  if (FAILED(scanned))
    return result;
  result["windowsEndpointsChecked"] = true;
  UINT count = 0;
  devices->GetCount(&count);
  constexpr PROPERTYKEY friendly{
      {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
  for (UINT i = 0; i < count; ++i) {
    IMMDevice* device = nullptr;
    if (FAILED(devices->Item(i, &device)))
      continue;
    IPropertyStore* properties = nullptr;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &properties))) {
      PROPVARIANT name{};
      if (SUCCEEDED(properties->GetValue(friendly, &name)) && name.vt == VT_LPWSTR &&
          name.pwszVal && std::wstring(name.pwszVal).find(L"OpenAoIP") != std::wstring::npos) {
        IMMEndpoint* endpoint = nullptr;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&endpoint)))) {
          EDataFlow flow = eAll;
          if (SUCCEEDED(endpoint->GetDataFlow(&flow))) {
            const auto key = flow == eRender ? "windowsRenderEndpoints" : "windowsCaptureEndpoints";
            result[key] = result[key].get<int>() + 1;
          }
          endpoint->Release();
        }
      }
      PropVariantClear(&name);
      properties->Release();
    }
    device->Release();
  }
  devices->Release();
  return result;
}
} // namespace aoip::vsc
#endif
