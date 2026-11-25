#include "aoip/virtual_bridge.hpp"
#ifdef _WIN32
#include <algorithm>
#include <cstring>
#include <stdexcept>
namespace aoip::vsc {
std::size_t ring_available(SharedRing& ring) {
  const auto r = load64(ring.read), w = load64(ring.write);
  return static_cast<std::size_t>(std::min<std::uint64_t>(w - r, capacity_frames));
}
std::size_t ring_write(SharedRing& ring, std::span<const float> samples, std::size_t channels) {
  if (!channels || channels > max_channels || samples.size() % channels)
    return 0;
  const auto w = load64(ring.write), r = load64(ring.read);
  const auto requested = samples.size() / channels;
  const auto frames = std::min<std::size_t>(
      requested,
      capacity_frames - static_cast<std::size_t>(std::min<std::uint64_t>(w - r, capacity_frames)));
  for (std::size_t f = 0; f < frames; ++f)
    std::copy_n(samples.data() + f * channels, channels,
                ring.data + ((w + f) % capacity_frames) * max_channels);
  MemoryBarrier();
  InterlockedExchange64(&ring.write, static_cast<LONG64>(w + frames));
  if (requested > frames)
    InterlockedAdd64(&ring.dropped, static_cast<LONG64>(requested - frames));
  return frames;
}
std::size_t ring_read(SharedRing& ring, std::span<float> samples, std::size_t channels) {
  if (!channels || channels > max_channels || samples.size() % channels)
    return 0;
  const auto r = load64(ring.read), w = load64(ring.write);
  const auto requested = samples.size() / channels;
  const auto frames = std::min<std::size_t>(
      requested, static_cast<std::size_t>(std::min<std::uint64_t>(w - r, capacity_frames)));
  for (std::size_t f = 0; f < frames; ++f)
    std::copy_n(ring.data + ((r + f) % capacity_frames) * max_channels, channels,
                samples.data() + f * channels);
  std::fill(samples.begin() + frames * channels, samples.end(), 0.0f);
  MemoryBarrier();
  InterlockedExchange64(&ring.read, static_cast<LONG64>(r + frames));
  if (requested > frames)
    InterlockedAdd64(&ring.underruns, static_cast<LONG64>(requested - frames));
  return frames;
}
struct BridgeMapping::Impl {
  struct Settings {volatile LONG preferred,active;};
  HANDLE settings_mapping=nullptr;
  Settings* settings=nullptr;
  HANDLE mapping = nullptr;
  HANDLE configuration_lock = nullptr;
  SharedAudio* data = nullptr;
  ~Impl() {
    if(settings)UnmapViewOfFile(settings);
    if(settings_mapping)CloseHandle(settings_mapping);
    if (data)
      UnmapViewOfFile(data);
    if (mapping)
      CloseHandle(mapping);
    if (configuration_lock)
      CloseHandle(configuration_lock);
  }
};
BridgeMapping::BridgeMapping(bool create, const std::wstring& name)
    : impl_(std::make_unique<Impl>()) {
  impl_->configuration_lock = CreateMutexW(nullptr, FALSE, (name + L".Configure").c_str());
  if (!impl_->configuration_lock)
    throw std::runtime_error("Cannot create virtual audio configuration lock");
  if (create)
    impl_->mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                        sizeof(SharedAudio), name.c_str());
  else
    impl_->mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
  if (!impl_->mapping)
    throw std::runtime_error("Start OpenAoIP Virtual Soundcard before opening its ASIO driver");
  const bool fresh = create && GetLastError() != ERROR_ALREADY_EXISTS;
  impl_->data = static_cast<SharedAudio*>(
      MapViewOfFile(impl_->mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedAudio)));
  if (!impl_->data)
    throw std::runtime_error("Cannot map virtual audio buffers");
  if (fresh) {
    std::memset(impl_->data, 0, sizeof(SharedAudio));
    impl_->data->version = bridge_version;
    impl_->data->size = sizeof(SharedAudio);
    MemoryBarrier();
    impl_->data->magic = bridge_magic;
  }
  if (impl_->data->magic != bridge_magic || impl_->data->version != bridge_version ||
      impl_->data->size != sizeof(SharedAudio))
    throw std::runtime_error("Virtual audio bridge version mismatch");
  const auto settings_name=name+L".Settings";
  impl_->settings_mapping=create?CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Impl::Settings),settings_name.c_str()):OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,settings_name.c_str());
  if(impl_->settings_mapping)impl_->settings=static_cast<Impl::Settings*>(MapViewOfFile(impl_->settings_mapping,FILE_MAP_ALL_ACCESS,0,0,sizeof(Impl::Settings)));
}
BridgeMapping::~BridgeMapping() = default;
SharedAudio* BridgeMapping::get() const {
  return impl_->data;
}
void BridgeMapping::set_preferred_buffer(int frames){
  if(frames<64||frames>1024||(frames&(frames-1)))throw std::runtime_error("ASIO buffer must be a power of two between 64 and 1024 frames");
  if(impl_->data->asio_pid&&preferred_buffer()!=frames)throw std::runtime_error("Close the ASIO host before changing its preferred buffer");
  if(!impl_->settings)throw std::runtime_error("Cannot map ASIO buffer settings");
  InterlockedExchange(&impl_->settings->preferred,frames);
}
int BridgeMapping::preferred_buffer()const{const int value=impl_->settings?InterlockedCompareExchange(&impl_->settings->preferred,0,0):256;return value>=64&&value<=1024&&!(value&(value-1))?value:256;}
int BridgeMapping::active_buffer()const{return impl_->settings?InterlockedCompareExchange(&impl_->settings->active,0,0):0;}
void BridgeMapping::publish_active_buffer(int frames){if(impl_->settings)InterlockedExchange(&impl_->settings->active,frames);}
void BridgeMapping::configure(int tx, int rx, int rate, int active_state) {
  if (tx < 1 || rx < 1 || tx > 32 || rx > 32 || (rate != 44100 && rate != 48000 && rate != 96000))
    throw std::runtime_error("Invalid virtual soundcard channel count or rate");
  if (active_state != 1 && active_state != 2)
    throw std::runtime_error("Invalid virtual soundcard backend state");
  auto& d = *impl_->data;
  const auto acquired = WaitForSingleObject(impl_->configuration_lock, 2000);
  if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED)
    throw std::runtime_error("Virtual audio configuration is busy");
  struct Unlock {
    HANDLE handle;
    ~Unlock() {
      ReleaseMutex(handle);
    }
  } unlock{impl_->configuration_lock};
  auto alive = [](LONG pid) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process)
      return GetLastError() != ERROR_INVALID_PARAMETER;
    const bool result = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return result;
  };
  const auto client = InterlockedCompareExchange(&d.asio_pid, 0, 0);
  if (client) {
    if (!alive(client))
      InterlockedCompareExchange(&d.asio_pid, 0, client);
    else if (active_state == 2)
      throw std::runtime_error("Close the ASIO host before switching to Windows audio");
    else if (d.tx_channels != static_cast<std::uint32_t>(tx) ||
             d.rx_channels != static_cast<std::uint32_t>(rx) ||
             d.sample_rate != static_cast<std::uint32_t>(rate))
      throw std::runtime_error("Close the ASIO host before changing channels or sample rate");
  }
  if (d.active && alive(d.owner_pid))
    throw std::runtime_error("Another Virtual Soundcard instance is already running");
  d.tx_channels = tx;
  d.rx_channels = rx;
  d.sample_rate = rate;
  d.owner_pid = GetCurrentProcessId();
  InterlockedExchange64(&d.to_network.read, load64(d.to_network.write));
  InterlockedExchange64(&d.from_network.read, load64(d.from_network.write));
  InterlockedExchange(&d.active, active_state);
}
} // namespace aoip::vsc
#endif
