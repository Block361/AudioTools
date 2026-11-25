// SPDX-License-Identifier: GPL-3.0-only
// Uses the Steinberg ASIO SDK under its GPLv3 option. See LICENSE.md.
#include "aoip/virtual_bridge.hpp"
#include "asio_api.hpp"
#include "driver_id.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <objbase.h>
#include <shellapi.h>
#include <stdexcept>
#include <thread>
#include <vector>
#include <windows.h>

namespace {
HMODULE module;
std::atomic<long> objects{0}, server_locks{0};
using aoip::vsc::BridgeMapping;
using aoip::vsc::SharedAudio;
template <class T> void timestamp(T& out, std::uint64_t value) {
  out.hi = static_cast<unsigned long>(value >> 32);
  out.lo = static_cast<unsigned long>(value);
}
class Driver final : public IASIO {
  std::atomic<ULONG> references{1};
  std::unique_ptr<BridgeMapping> mapping;
  ASIOCallbacks callbacks{};
  std::vector<ASIOBufferInfo> buffers;
  std::vector<std::vector<float>> storage;
  std::vector<float> incoming, outgoing;
  std::thread worker;
  HANDLE stop_event = nullptr;
  std::atomic<bool> started{false};
  std::atomic<std::uint64_t> position{0}, time_ns{0};
  long block_size = 256;
  bool time_info = false, claimed = false;
  char error[124] = "Start OpenAoIP Virtual Soundcard, then select this driver.";

public:
  Driver() {
    ++objects;
    stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }
  ~Driver() {
    disposeBuffers();
    if (claimed && mapping)
      InterlockedCompareExchange(&mapping->get()->asio_pid, 0, GetCurrentProcessId());
    if (stop_event)
      CloseHandle(stop_event);
    --objects;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** result) override {
    if (!result)
      return E_POINTER;
    *result = nullptr;
    if (id == IID_IUnknown || id == CLSID_OpenAoIP_ASIO) {
      *result = static_cast<IASIO*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return ++references;
  }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto count = --references;
    if (!count)
      delete this;
    return count;
  }
  ASIOBool init(void*) override {
    if (mapping)
      return ASIOTrue;
    try {
      wchar_t test_name[256]{};
      GetEnvironmentVariableW(L"OPENAOIP_VSC_TEST_MAPPING", test_name, 256);
      const auto name = std::wstring(test_name);
      mapping = std::make_unique<BridgeMapping>(
          false, name.starts_with(L"Local\\OpenAoIP.Test.") ? name : aoip::vsc::mapping_name);
      auto* d = mapping->get();
      if (d->active != 1 || !d->tx_channels || !d->rx_channels || d->tx_channels > 32 ||
          d->rx_channels > 32)
        throw std::runtime_error("Virtual Soundcard is stopped or not configured");
      if (InterlockedCompareExchange(&d->asio_pid, GetCurrentProcessId(), 0) != 0)
        throw std::runtime_error("Only one ASIO host can use this soundcard at a time");
      claimed = true;
      return ASIOTrue;
    } catch (const std::exception& e) {
      strncpy_s(error, e.what(), _TRUNCATE);
      mapping.reset();
      return ASIOFalse;
    }
  }
  void getDriverName(char* name) override {
    strcpy_s(name, 32, "OpenAoIP Virtual Soundcard");
  }
  long getDriverVersion() override {
    return 400;
  }
  void getErrorMessage(char* text) override {
    strcpy_s(text, 124, error);
  }
  ASIOError getChannels(long* inputs, long* outputs) override {
    if (!mapping || !inputs || !outputs)
      return ASE_NotPresent;
    *inputs = mapping->get()->rx_channels;
    *outputs = mapping->get()->tx_channels;
    return ASE_OK;
  }
  ASIOError getLatencies(long* input, long* output) override {
    if (!mapping || !input || !output)
      return ASE_NotPresent;
    *input = block_size + mapping->get()->sample_rate * mapping->get()->latency_ms / 1000;
    *output = block_size;
    return ASE_OK;
  }
  ASIOError getBufferSize(long* minimum, long* maximum, long* preferred,
                          long* granularity) override {
    if (!minimum || !maximum || !preferred || !granularity)
      return ASE_InvalidParameter;
    *minimum = 64;
    *maximum = 1024;
    *preferred = mapping?mapping->preferred_buffer():256;
    *granularity = -1;
    return ASE_OK;
  }
  ASIOError canSampleRate(ASIOSampleRate rate) override {
    return mapping && rate == mapping->get()->sample_rate ? ASE_OK : ASE_NoClock;
  }
  ASIOError getSampleRate(ASIOSampleRate* rate) override {
    if (!mapping || !rate)
      return ASE_NotPresent;
    *rate = mapping->get()->sample_rate;
    return ASE_OK;
  }
  ASIOError setSampleRate(ASIOSampleRate rate) override {
    return canSampleRate(rate);
  }
  ASIOError getClockSources(ASIOClockSource* clocks, long* count) override {
    if (!clocks || !count || *count < 1)
      return ASE_InvalidParameter;
    *count = 1;
    clocks[0] = {};
    clocks[0].index = 0;
    clocks[0].associatedChannel = -1;
    clocks[0].associatedGroup = -1;
    clocks[0].isCurrentSource = ASIOTrue;
    strcpy_s(clocks[0].name, "Software clock (not PTP locked)");
    return ASE_OK;
  }
  ASIOError setClockSource(long reference) override {
    return reference == 0 ? ASE_OK : ASE_InvalidParameter;
  }
  ASIOError getSamplePosition(ASIOSamples* samples, ASIOTimeStamp* time) override {
    if (!samples || !time)
      return ASE_InvalidParameter;
    timestamp(*samples, position.load());
    timestamp(*time, time_ns.load());
    return ASE_OK;
  }
  ASIOError getChannelInfo(ASIOChannelInfo* info) override {
    if (!mapping || !info)
      return ASE_NotPresent;
    const auto count = info->isInput ? mapping->get()->rx_channels : mapping->get()->tx_channels;
    if (info->channel < 0 || static_cast<unsigned long>(info->channel) >= count)
      return ASE_InvalidParameter;
    info->isActive = ASIOFalse;
    for (const auto& b : buffers)
      if (b.channelNum == info->channel && b.isInput == info->isInput)
        info->isActive = ASIOTrue;
    info->channelGroup = info->channel / 2;
    info->type = ASIOSTFloat32LSB;
    sprintf_s(info->name, "OpenAoIP %s %ld", info->isInput ? "Input" : "Output", info->channel + 1);
    return ASE_OK;
  }
  ASIOError createBuffers(ASIOBufferInfo* info, long count, long size, ASIOCallbacks* cb) override {
    if (!mapping || !info || !cb || !cb->bufferSwitch || count < 1 || count > 64 || size < 64 ||
        size > 1024 || (size & (size - 1)))
      return ASE_InvalidParameter;
    if (started)
      return ASE_InvalidMode;
    for (long i = 0; i < count; ++i) {
      const auto channels =
          info[i].isInput ? mapping->get()->rx_channels : mapping->get()->tx_channels;
      if (info[i].channelNum < 0 || static_cast<unsigned long>(info[i].channelNum) >= channels)
        return ASE_InvalidParameter;
      for (long j = 0; j < i; ++j)
        if (info[i].isInput == info[j].isInput && info[i].channelNum == info[j].channelNum)
          return ASE_InvalidParameter;
    }
    try {
      buffers.assign(info, info + count);
      storage.clear();
      storage.resize(count);
      for (long i = 0; i < count; ++i) {
        storage[i].assign(size * 2, 0);
        info[i].buffers[0] = storage[i].data();
        info[i].buffers[1] = storage[i].data() + size;
        buffers[i] = info[i];
      }
      block_size = size;
      mapping->publish_active_buffer(size);
      callbacks = *cb;
      incoming.assign(size * mapping->get()->rx_channels, 0);
      outgoing.assign(size * mapping->get()->tx_channels, 0);
      time_info = cb->bufferSwitchTimeInfo && cb->asioMessage &&
                  cb->asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr) == 1;
      return ASE_OK;
    } catch (...) {
      buffers.clear();
      storage.clear();
      return ASE_NoMemory;
    }
  }
  ASIOError disposeBuffers() override {
    if (worker.joinable() && worker.get_id() == std::this_thread::get_id())
      return ASE_InvalidMode;
    stop();
    buffers.clear();
    storage.clear();
    callbacks = {};
    if(mapping)mapping->publish_active_buffer(0);
    return ASE_OK;
  }
  ASIOError start() override {
    if (!mapping || buffers.empty() || !stop_event || !mapping->get()->active)
      return ASE_NotPresent;
    if (started.exchange(true))
      return ASE_InvalidMode;
    ResetEvent(stop_event);
    position = 0;
    try {
      worker = std::thread([this] { run(); });
      return ASE_OK;
    } catch (...) {
      started = false;
      return ASE_NoMemory;
    }
  }
  ASIOError stop() override {
    if (started.exchange(false))
      SetEvent(stop_event);
    if (worker.joinable()) {
      if (worker.get_id() == std::this_thread::get_id())
        return ASE_InvalidMode;
      worker.join();
    }
    return ASE_OK;
  }
  ASIOError controlPanel() override {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(module, path, MAX_PATH);
    auto text = std::wstring(path);
    const auto slash = text.find_last_of(L"\\/");
    text.resize(slash + 1);
    const auto legacy=text+L"OpenAoIPVirtualSoundcard.exe";
    text += L"OpenAoIP Virtual Soundcard.exe";
    if(GetFileAttributesW(text.c_str())==INVALID_FILE_ATTRIBUTES)text=legacy;
    return reinterpret_cast<INT_PTR>(
               ShellExecuteW(nullptr, L"open", text.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32
               ? ASE_OK
               : ASE_NotPresent;
  }
  ASIOError future(long selector, void*) override {
    return selector == kAsioCanTimeInfo ? ASE_SUCCESS : ASE_NotPresent;
  }
  ASIOError outputReady() override {
    return ASE_NotPresent;
  }

private:
  void run() {
    LARGE_INTEGER frequency{}, counter{};
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    double deadline = static_cast<double>(counter.QuadPart);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
    if (!timer)
      timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    const double step = block_size * double(frequency.QuadPart) / mapping->get()->sample_rate;
    long index = 0;
    auto* d = mapping->get();
    while (started) {
      if (d->active)
        aoip::vsc::ring_read(d->from_network, incoming, d->rx_channels);
      else
        std::fill(incoming.begin(), incoming.end(), 0.f);
      for (std::size_t i = 0; i < buffers.size(); ++i)
        if (buffers[i].isInput) {
          auto* out = static_cast<float*>(buffers[i].buffers[index]);
          for (long f = 0; f < block_size; ++f)
            out[f] = incoming[f * d->rx_channels + buffers[i].channelNum];
        }
      QueryPerformanceCounter(&counter);
      const auto ns =
          static_cast<std::uint64_t>(counter.QuadPart * (1000000000.0 / frequency.QuadPart));
      time_ns = ns;
      if (time_info) {
        ASIOTime time{};
        time.timeInfo.speed = 1;
        timestamp(time.timeInfo.systemTime, ns);
        timestamp(time.timeInfo.samplePosition, position.load());
        time.timeInfo.sampleRate = d->sample_rate;
        time.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;
        callbacks.bufferSwitchTimeInfo(&time, index, ASIOFalse);
      } else
        callbacks.bufferSwitch(index, ASIOFalse);
      std::fill(outgoing.begin(), outgoing.end(), 0.f);
      for (const auto& b : buffers)
        if (!b.isInput) {
          const auto* input = static_cast<const float*>(b.buffers[index]);
          for (long f = 0; f < block_size; ++f)
            outgoing[f * d->tx_channels + b.channelNum] = input[f];
        }
      if (d->active)
        aoip::vsc::ring_write(d->to_network, outgoing, d->tx_channels);
      InterlockedIncrement64(&d->asio_callbacks);
      position += block_size;
      index ^= 1;
      deadline += step;
      QueryPerformanceCounter(&counter);
      if (deadline < counter.QuadPart - step * 4)
        deadline = counter.QuadPart;
      const auto remaining = deadline - counter.QuadPart;
      if (remaining > 0 && timer) {
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<LONGLONG>(
            1, static_cast<LONGLONG>(remaining * 10000000 / frequency.QuadPart));
        SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
        HANDLE handles[2] = {stop_event, timer};
        if (WaitForMultipleObjects(2, handles, FALSE, 1000) == WAIT_OBJECT_0)
          break;
      } else if (WaitForSingleObject(stop_event, 1) == WAIT_OBJECT_0)
        break;
    }
    if (timer)
      CloseHandle(timer);
  }
};
class Factory final : public IClassFactory {
  std::atomic<ULONG> refs{1};

public:
  Factory() {
    ++objects;
  }
  ~Factory() {
    --objects;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
    if (!out)
      return E_POINTER;
    *out = nullptr;
    if (id == IID_IUnknown || id == IID_IClassFactory) {
      *out = this;
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override {
    return ++refs;
  }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto r = --refs;
    if (!r)
      delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID id, void** out) override {
    if (outer)
      return CLASS_E_NOAGGREGATION;
    try {
      auto* driver = new Driver;
      const auto hr = driver->QueryInterface(id, out);
      driver->Release();
      return hr;
    } catch (...) {
      return E_OUTOFMEMORY;
    }
  }
  HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
    if (lock)
      ++server_locks;
    else
      --server_locks;
    return S_OK;
  }
};
LONG set_registry(const std::wstring& key, const wchar_t* name, const std::wstring& value) {
  HKEY handle{};
  auto result = RegCreateKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, nullptr, 0,
                                KEY_WRITE | KEY_WOW64_64KEY, nullptr, &handle, nullptr);
  if (result != ERROR_SUCCESS)
    return result;
  result = RegSetValueExW(handle, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                          static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
  RegCloseKey(handle);
  return result;
}
} // namespace
extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH)
    module = instance;
  return TRUE;
}
STDAPI DllGetClassObject(REFCLSID clsid, REFIID id, void** out) {
  if (clsid != CLSID_OpenAoIP_ASIO)
    return CLASS_E_CLASSNOTAVAILABLE;
  try {
    auto* factory = new Factory;
    const auto hr = factory->QueryInterface(id, out);
    factory->Release();
    return hr;
  } catch (...) {
    return E_OUTOFMEMORY;
  }
}
STDAPI DllCanUnloadNow() {
  return objects == 0 && server_locks == 0 ? S_OK : S_FALSE;
}
STDAPI DllRegisterServer() {
  wchar_t path[MAX_PATH]{};
  GetModuleFileNameW(module, path, MAX_PATH);
  const std::wstring clsid = asio_clsid_text;
  LONG result =
      set_registry(L"SOFTWARE\\Classes\\CLSID\\" + clsid, nullptr, L"OpenAoIP Virtual Soundcard");
  if (result == ERROR_SUCCESS)
    result =
        set_registry(L"SOFTWARE\\Classes\\CLSID\\" + clsid + L"\\InprocServer32", nullptr, path);
  if (result == ERROR_SUCCESS)
    result = set_registry(L"SOFTWARE\\Classes\\CLSID\\" + clsid + L"\\InprocServer32",
                          L"ThreadingModel", L"Both");
  if (result == ERROR_SUCCESS)
    result = set_registry(L"SOFTWARE\\ASIO\\OpenAoIP Virtual Soundcard", L"CLSID", clsid);
  if (result == ERROR_SUCCESS)
    result = set_registry(L"SOFTWARE\\ASIO\\OpenAoIP Virtual Soundcard", L"Description",
                          L"OpenAoIP Virtual Soundcard");
  return HRESULT_FROM_WIN32(result);
}
STDAPI DllUnregisterServer() {
  HKEY software{};
  LONG r =
      RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE", 0, KEY_WRITE | KEY_WOW64_64KEY, &software);
  if (r != ERROR_SUCCESS)
    return HRESULT_FROM_WIN32(r);
  RegDeleteTreeW(software, L"ASIO\\OpenAoIP Virtual Soundcard");
  RegDeleteTreeW(software, (L"Classes\\CLSID\\" + std::wstring(asio_clsid_text)).c_str());
  RegCloseKey(software);
  return S_OK;
}
