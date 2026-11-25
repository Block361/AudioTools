// SPDX-License-Identifier: MIT
// Experimental PortCls/WaveRT driver. Build/INF validation does not establish
// kernel runtime safety. Deploy only after signing and testing on a dedicated VM.
// clang-format off
#include <ntifs.h>
#include <initguid.h>
#include <portcls.h>
#include <wdmsec.h>
#include "protocol.h"
#include "audio_queue.hpp"
// clang-format on

static constexpr ULONG tag = 'pIoA';
static PDEVICE_OBJECT control_device;
static PDRIVER_DISPATCH previous[IRP_MJ_MAXIMUM_FUNCTION + 1];
static PDRIVER_UNLOAD previous_unload;
static KSPIN_LOCK queue_lock;
static PFILE_OBJECT owner_file;
static ULONG owner_pid;
static OPENAOIP_WDM_CONFIG configuration = {OPENAOIP_WDM_VERSION, 2, 2, OPENAOIP_WDM_RATE};
static volatile LONG render_mask, capture_mask;
static aoip_wdm::Queue render_queue[OPENAOIP_WDM_PAIRS], capture_queue[OPENAOIP_WDM_PAIRS];
static bool valid_rate(ULONG rate) {
  return rate == OPENAOIP_WDM_RATE;
}
void* __cdecl operator new(size_t bytes) {
  return ExAllocatePool2(POOL_FLAG_NON_PAGED, bytes, tag);
}
void __cdecl operator delete(void* p) noexcept {
  if (p)
    ExFreePoolWithTag(p, tag);
}
void __cdecl operator delete(void* p, size_t) noexcept {
  if (p)
    ExFreePoolWithTag(p, tag);
}
extern "C" int _purecall() {
  return 0;
}
static GUID endpoint_name(bool capture, ULONG pair) {
  return {static_cast<ULONG>((capture ? 0x9f65c000 : 0x9f65b000) + pair),
          0x38dc,
          0x4427,
          {0xa3, 0x9e, 0xc7, 0x10, 0x6d, 0x98, 0x18, 0x65}};
}
static bool valid_format(const KSDATAFORMAT* data, ULONG size) {
  if (!data || size < sizeof(KSDATAFORMAT_WAVEFORMATEX) || data->FormatSize > size ||
      !IsEqualGUIDAligned(data->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) ||
      !IsEqualGUIDAligned(data->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) ||
      !IsEqualGUIDAligned(data->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX))
    return false;
  const auto& f = reinterpret_cast<const KSDATAFORMAT_WAVEFORMATEX*>(data)->WaveFormatEx;
  if (f.nChannels != 2 || f.nSamplesPerSec != OPENAOIP_WDM_RATE || f.wBitsPerSample != 16 ||
      f.nBlockAlign != 4 || f.nAvgBytesPerSec != OPENAOIP_WDM_RATE * 4)
    return false;
  if (f.wFormatTag == WAVE_FORMAT_PCM)
    return true;
  if (f.wFormatTag != WAVE_FORMAT_EXTENSIBLE || f.cbSize < 22 ||
      size < sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE))
    return false;
  const auto& e = reinterpret_cast<const KSDATAFORMAT_WAVEFORMATEXTENSIBLE*>(data)->WaveFormatExt;
  return e.Samples.wValidBitsPerSample == 16 &&
         (e.dwChannelMask == 0 || e.dwChannelMask == KSAUDIO_SPEAKER_STEREO) &&
         IsEqualGUIDAligned(e.SubFormat, KSDATAFORMAT_SUBTYPE_PCM);
}
static NTSTATUS property_support(PPCPROPERTY_REQUEST request, ULONG flags) {
  if (!request->ValueSize) {
    request->ValueSize = sizeof(ULONG);
    return STATUS_BUFFER_OVERFLOW;
  }
  if (request->ValueSize < sizeof(ULONG) || !request->Value)
    return STATUS_BUFFER_TOO_SMALL;
  *static_cast<ULONG*>(request->Value) = flags | KSPROPERTY_TYPE_BASICSUPPORT;
  request->ValueSize = sizeof(ULONG);
  return STATUS_SUCCESS;
}
static NTSTATUS proposed_format(PPCPROPERTY_REQUEST request) {
  if (request->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
    return property_support(request, KSPROPERTY_TYPE_SET);
  if (!(request->Verb & KSPROPERTY_TYPE_SET))
    return STATUS_INVALID_DEVICE_REQUEST;
  return valid_format(static_cast<const KSDATAFORMAT*>(request->Value), request->ValueSize)
             ? STATUS_SUCCESS
             : STATUS_NO_MATCH;
}
static const PCPROPERTY_ITEM wave_properties[] = {
    {&KSPROPSETID_Pin, KSPROPERTY_PIN_PROPOSEDATAFORMAT,
     KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT, proposed_format}};
DEFINE_PCAUTOMATION_TABLE_PROP(wave_automation, wave_properties);

class Stream final : public IMiniportWaveRTStreamNotification {
  LONG refs = 1;
  PPORTWAVERTSTREAM port;
  bool capture;
  ULONG pair;
  volatile KSSTATE state = KSSTATE_STOP;
  WAVEFORMATEX format{};
  PMDL mdl = nullptr;
  PUCHAR buffer = nullptr;
  ULONG bytes = 0, notifications = 0;
  ULONGLONG position = 0, origin_bytes = 0;
  LARGE_INTEGER origin{}, frequency{};
  KTIMER timer;
  KDPC dpc;
  KSPIN_LOCK event_lock;
  PKEVENT events[8]{};
  static void NTAPI on_timer(KDPC*, PVOID context, PVOID, PVOID) {
    static_cast<Stream*>(context)->tick();
  }
  void stop_timer() {
    KeCancelTimer(&timer);
    KeRemoveQueueDpc(&dpc);
    KeFlushQueuedDpcs();
  }
  void tick() {
    if (state != KSSTATE_RUN || !buffer || !bytes)
      return;
    const auto time = KeQueryPerformanceCounter(nullptr);
    ULONGLONG target =
        origin_bytes + static_cast<ULONGLONG>((time.QuadPart - origin.QuadPart) *
                                              format.nSamplesPerSec / frequency.QuadPart) *
                           format.nBlockAlign;
    if (target < position)
      return;
    ULONGLONG available = target - position;
    const auto maximum = min(bytes, OPENAOIP_WDM_BLOCK_FRAMES * 4u);
    if (available > maximum) {
      position = target - maximum;
      available = maximum;
    }
    available -= available % format.nBlockAlign;
    KIRQL level;
    KeAcquireSpinLock(&queue_lock, &level);
    for (ULONGLONG offset = 0; offset < available; offset += format.nBlockAlign) {
      auto* frame = reinterpret_cast<SHORT*>(buffer + ((position + offset) % bytes));
      if (capture) {
        if (owner_pid)
          capture_queue[pair].pop(frame, 1);
        else
          RtlZeroMemory(frame, format.nBlockAlign);
      } else if (owner_pid)
        render_queue[pair].push(frame, 1);
    }
    KeReleaseSpinLock(&queue_lock, level);
    const auto old = position;
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&position),
                          static_cast<LONG64>(position + available));
    const ULONG period = notifications ? bytes / notifications : bytes;
    if (period && old / period != position / period) {
      KeAcquireSpinLock(&event_lock, &level);
      for (auto* event : events)
        if (event)
          KeSetEvent(event, IO_SOUND_INCREMENT, FALSE);
      KeReleaseSpinLock(&event_lock, level);
    }
  }

public:
  Stream(PPORTWAVERTSTREAM p, bool c, ULONG index) : port(p), capture(c), pair(index) {
    port->AddRef();
    KeInitializeTimer(&timer);
    KeInitializeDpc(&dpc, on_timer, this);
    KeInitializeSpinLock(&event_lock);
    KeQueryPerformanceCounter(&frequency);
  }
  ~Stream() {
    SetState(KSSTATE_STOP);
    FreeAudioBuffer(mdl, bytes);
    port->Release();
  }
  STDMETHODIMP_(NTSTATUS) QueryInterface(REFIID id, PVOID* out) override {
    if (!out)
      return STATUS_INVALID_PARAMETER;
    *out = nullptr;
    if (IsEqualGUIDAligned(id, IID_IUnknown) || IsEqualGUIDAligned(id, IID_IMiniportWaveRTStream) ||
        IsEqualGUIDAligned(id, IID_IMiniportWaveRTStreamNotification)) {
      *out = static_cast<IMiniportWaveRTStreamNotification*>(this);
      AddRef();
      return STATUS_SUCCESS;
    }
    return STATUS_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override {
    return InterlockedIncrement(&refs);
  }
  STDMETHODIMP_(ULONG) Release() override {
    const auto n = InterlockedDecrement(&refs);
    if (!n)
      delete this;
    return n;
  }
  STDMETHODIMP_(NTSTATUS) SetFormat(PKSDATAFORMAT data) override {
    if (state == KSSTATE_RUN || !data || data->FormatSize < sizeof(KSDATAFORMAT_WAVEFORMATEX))
      return STATUS_INVALID_DEVICE_STATE;
    if (!valid_format(data, data->FormatSize))
      return STATUS_NO_MATCH;
    format = reinterpret_cast<PKSDATAFORMAT_WAVEFORMATEX>(data)->WaveFormatEx;
    return STATUS_SUCCESS;
  }
  STDMETHODIMP_(NTSTATUS) SetState(KSSTATE next) override {
    if (next > KSSTATE_RUN)
      return STATUS_INVALID_PARAMETER;
    if (state == KSSTATE_RUN && next != KSSTATE_RUN) {
      state = next;
      stop_timer();
      InterlockedAnd(capture ? &capture_mask : &render_mask, ~(1L << pair));
    }
    if (next == KSSTATE_STOP) {
      position = origin_bytes = 0;
    }
    if (next == KSSTATE_RUN && state != KSSTATE_RUN) {
      if (!buffer || !format.nBlockAlign)
        return STATUS_INVALID_DEVICE_STATE;
      KIRQL level;
      KeAcquireSpinLock(&queue_lock, &level);
      (capture ? capture_queue[pair] : render_queue[pair]).clear();
      origin = KeQueryPerformanceCounter(&frequency);
      origin_bytes = position;
      state = next;
      InterlockedOr(capture ? &capture_mask : &render_mask, 1L << pair);
      KeReleaseSpinLock(&queue_lock, level);
      LARGE_INTEGER due{};
      due.QuadPart = -10000;
      KeSetTimerEx(&timer, due, 1, &dpc);
    }
    state = next;
    return STATUS_SUCCESS;
  }
  STDMETHODIMP_(NTSTATUS) GetPosition(PKSAUDIO_POSITION out) override {
    if (!out || !bytes)
      return STATUS_INVALID_DEVICE_STATE;
    const auto p = static_cast<ULONGLONG>(
        InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&position), 0, 0));
    out->PlayOffset = p % bytes;
    out->WriteOffset = (p + format.nBlockAlign * (format.nSamplesPerSec / 200)) % bytes;
    return STATUS_SUCCESS;
  }
  STDMETHODIMP_(NTSTATUS)
  AllocateAudioBuffer(ULONG requested, PMDL* out, PULONG actual, PULONG offset,
                      MEMORY_CACHING_TYPE* cache) override {
    if (!out || !actual || !offset || !cache || !format.nBlockAlign || mdl ||
        requested < format.nBlockAlign || requested > 1024 * 1024)
      return STATUS_INVALID_PARAMETER;
    requested -= requested % format.nBlockAlign;
    PHYSICAL_ADDRESS highest{};
    highest.QuadPart = MAXLONGLONG;
    mdl = port->AllocatePagesForMdl(highest, requested);
    if (!mdl)
      return STATUS_INSUFFICIENT_RESOURCES;
    buffer = static_cast<PUCHAR>(port->MapAllocatedPages(mdl, MmCached));
    if (!buffer) {
      port->FreePagesFromMdl(mdl);
      mdl = nullptr;
      return STATUS_INSUFFICIENT_RESOURCES;
    }
    bytes = requested;
    RtlZeroMemory(buffer, bytes);
    *out = mdl;
    *actual = bytes;
    *offset = 0;
    *cache = MmCached;
    return STATUS_SUCCESS;
  }
  STDMETHODIMP_(void) FreeAudioBuffer(PMDL memory, ULONG) override {
    if (!memory || memory != mdl)
      return;
    SetState(KSSTATE_STOP);
    port->UnmapAllocatedPages(buffer, mdl);
    port->FreePagesFromMdl(mdl);
    buffer = nullptr;
    mdl = nullptr;
    bytes = 0;
  }
  STDMETHODIMP_(void) GetHWLatency(KSRTAUDIO_HWLATENCY* out) override {
    if (out) {
      out->FifoSize = 0;
      out->ChipsetDelay = 0;
      out->CodecDelay = 0;
    }
  }
  STDMETHODIMP_(NTSTATUS) GetPositionRegister(KSRTAUDIO_HWREGISTER*) override {
    return STATUS_NOT_SUPPORTED;
  }
  STDMETHODIMP_(NTSTATUS) GetClockRegister(KSRTAUDIO_HWREGISTER*) override {
    return STATUS_NOT_SUPPORTED;
  }
  STDMETHODIMP_(NTSTATUS)
  AllocateBufferWithNotification(ULONG count, ULONG size, PMDL* out, PULONG actual, PULONG offset,
                                 MEMORY_CACHING_TYPE* cache) override {
    if (!count || count > 32 || size < count * 4)
      return STATUS_INVALID_PARAMETER;
    notifications = count;
    size -= size % (count * 4);
    return AllocateAudioBuffer(size, out, actual, offset, cache);
  }
  STDMETHODIMP_(void) FreeBufferWithNotification(PMDL memory, ULONG size) override {
    FreeAudioBuffer(memory, size);
    notifications = 0;
  }
  STDMETHODIMP_(NTSTATUS) RegisterNotificationEvent(PKEVENT event) override {
    if (!event)
      return STATUS_INVALID_PARAMETER;
    KIRQL level;
    KeAcquireSpinLock(&event_lock, &level);
    NTSTATUS result = STATUS_INSUFFICIENT_RESOURCES;
    for (auto& slot : events)
      if (slot == event) {
        result = STATUS_SUCCESS;
        break;
      } else if (!slot) {
        slot = event;
        result = STATUS_SUCCESS;
        break;
      }
    KeReleaseSpinLock(&event_lock, level);
    return result;
  }
  STDMETHODIMP_(NTSTATUS) UnregisterNotificationEvent(PKEVENT event) override {
    KIRQL level;
    KeAcquireSpinLock(&event_lock, &level);
    for (auto& slot : events)
      if (slot == event)
        slot = nullptr;
    KeReleaseSpinLock(&event_lock, level);
    return STATUS_SUCCESS;
  }
};

class Wave final : public IMiniportWaveRT {
  LONG refs = 1;
  bool capture;
  ULONG pair;
  PCFILTER_DESCRIPTOR descriptor{};
  PCPIN_DESCRIPTOR pins[2]{};
  KSDATARANGE_AUDIO audio{};
  KSDATARANGE analog{};
  PKSDATARANGE audio_ranges[1]{}, analog_ranges[1]{};
  PCCONNECTION_DESCRIPTOR connection{};

public:
  explicit Wave(bool c, ULONG index) : capture(c), pair(index) {
    audio.DataRange.FormatSize = sizeof(audio);
    audio.DataRange.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    audio.DataRange.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    audio.DataRange.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    audio.MaximumChannels = 2;
    audio.MinimumBitsPerSample = audio.MaximumBitsPerSample = 16;
    audio.MinimumSampleFrequency = audio.MaximumSampleFrequency = OPENAOIP_WDM_RATE;
    analog.FormatSize = sizeof(analog);
    analog.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    analog.SubFormat = KSDATAFORMAT_SUBTYPE_ANALOG;
    analog.Specifier = KSDATAFORMAT_SPECIFIER_NONE;
    audio_ranges[0] = reinterpret_cast<PKSDATARANGE>(&audio);
    analog_ranges[0] = &analog;
    const int stream = c ? 1 : 0, bridge = c ? 0 : 1;
    pins[stream].MaxGlobalInstanceCount = pins[stream].MaxFilterInstanceCount = 1;
    pins[stream].KsPinDescriptor.DataRangesCount = 1;
    pins[stream].KsPinDescriptor.DataRanges = audio_ranges;
    pins[stream].KsPinDescriptor.DataFlow = c ? KSPIN_DATAFLOW_OUT : KSPIN_DATAFLOW_IN;
    pins[stream].KsPinDescriptor.Communication = KSPIN_COMMUNICATION_SINK;
    pins[stream].KsPinDescriptor.Category = &KSCATEGORY_AUDIO;
    pins[bridge].KsPinDescriptor.DataRangesCount = 1;
    pins[bridge].KsPinDescriptor.DataRanges = analog_ranges;
    pins[bridge].KsPinDescriptor.DataFlow = c ? KSPIN_DATAFLOW_IN : KSPIN_DATAFLOW_OUT;
    pins[bridge].KsPinDescriptor.Communication = KSPIN_COMMUNICATION_NONE;
    pins[bridge].KsPinDescriptor.Category = &KSCATEGORY_AUDIO;
    connection = {PCFILTER_NODE, 0, PCFILTER_NODE, 1};
    descriptor.PinSize = sizeof(PCPIN_DESCRIPTOR);
    descriptor.AutomationTable = &wave_automation;
    descriptor.PinCount = 2;
    descriptor.Pins = pins;
    descriptor.ConnectionCount = 1;
    descriptor.Connections = &connection;
  }
  STDMETHODIMP_(NTSTATUS) QueryInterface(REFIID id, PVOID* out) override {
    if (!out)
      return STATUS_INVALID_PARAMETER;
    *out = nullptr;
    if (IsEqualGUIDAligned(id, IID_IUnknown) || IsEqualGUIDAligned(id, IID_IMiniport) ||
        IsEqualGUIDAligned(id, IID_IMiniportWaveRT)) {
      *out = static_cast<IMiniportWaveRT*>(this);
      AddRef();
      return STATUS_SUCCESS;
    }
    return STATUS_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override {
    return InterlockedIncrement(&refs);
  }
  STDMETHODIMP_(ULONG) Release() override {
    auto n = InterlockedDecrement(&refs);
    if (!n)
      delete this;
    return n;
  }
  STDMETHODIMP_(NTSTATUS) GetDescription(PPCFILTER_DESCRIPTOR* out) override {
    *out = &descriptor;
    return STATUS_SUCCESS;
  }
  STDMETHODIMP_(NTSTATUS)
  DataRangeIntersection(ULONG pin, PKSDATARANGE data, PKSDATARANGE, ULONG length, PVOID out,
                        PULONG needed) override {
    if (!needed || pin != (capture ? 1u : 0u) || !data)
      return STATUS_NO_MATCH;
    if (data->FormatSize < sizeof(KSDATARANGE_AUDIO) ||
        !IsEqualGUIDAligned(data->MajorFormat, KSDATAFORMAT_TYPE_AUDIO) ||
        (!IsEqualGUIDAligned(data->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) &&
         !IsEqualGUIDAligned(data->SubFormat, KSDATAFORMAT_SUBTYPE_WILDCARD)))
      return STATUS_NO_MATCH;
    const auto* range = reinterpret_cast<const KSDATARANGE_AUDIO*>(data);
    if (range->MaximumChannels < 2 || range->MinimumBitsPerSample > 16 ||
        range->MaximumBitsPerSample < 16 || range->MinimumSampleFrequency > OPENAOIP_WDM_RATE ||
        range->MaximumSampleFrequency < OPENAOIP_WDM_RATE)
      return STATUS_NO_MATCH;
    *needed = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
    if (!length)
      return STATUS_BUFFER_OVERFLOW;
    if (length < *needed)
      return STATUS_BUFFER_TOO_SMALL;
    if (!out)
      return STATUS_INVALID_PARAMETER;
    auto* result = static_cast<PKSDATAFORMAT_WAVEFORMATEXTENSIBLE>(out);
    RtlZeroMemory(result, sizeof(*result));
    result->DataFormat.FormatSize = sizeof(*result);
    result->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    result->DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    result->DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    auto& f = result->WaveFormatExt.Format;
    f.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.nChannels = 2;
    f.nSamplesPerSec = OPENAOIP_WDM_RATE;
    f.wBitsPerSample = 16;
    f.nBlockAlign = f.nChannels * 2;
    f.nAvgBytesPerSec = f.nSamplesPerSec * f.nBlockAlign;
    f.cbSize = 22;
    result->WaveFormatExt.Samples.wValidBitsPerSample = 16;
    result->WaveFormatExt.dwChannelMask = f.nChannels == 2 ? KSAUDIO_SPEAKER_STEREO : 0;
    result->WaveFormatExt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    result->DataFormat.SampleSize = f.nBlockAlign;
    return STATUS_SUCCESS;
  }
  STDMETHODIMP_(NTSTATUS) Init(PUNKNOWN, PRESOURCELIST, PPORTWAVERT) override {
    return STATUS_SUCCESS;
  }
  STDMETHODIMP_(NTSTATUS) GetDeviceDescription(PDEVICE_DESCRIPTION out) override {
    RtlZeroMemory(out, sizeof(*out));
    out->Version = DEVICE_DESCRIPTION_VERSION;
    out->Master = TRUE;
    out->ScatterGather = TRUE;
    out->Dma32BitAddresses = TRUE;
    out->InterfaceType = PCIBus;
    out->MaximumLength = 1024 * 1024;
    return STATUS_SUCCESS;
  }
  STDMETHODIMP_(NTSTATUS)
  NewStream(PMINIPORTWAVERTSTREAM* out, PPORTWAVERTSTREAM port, ULONG pin, BOOLEAN is_capture,
            PKSDATAFORMAT format) override {
    if (!out || pin != (capture ? 1u : 0u) || bool(is_capture) != capture)
      return STATUS_INVALID_PARAMETER;
    *out = nullptr;
    auto* stream = new Stream(port, capture, pair);
    if (!stream)
      return STATUS_INSUFFICIENT_RESOURCES;
    const auto status = stream->SetFormat(format);
    if (!NT_SUCCESS(status)) {
      stream->Release();
      return status;
    }
    *out = stream;
    return STATUS_SUCCESS;
  }
};

class Topology final : public IMiniportTopology {
  LONG refs = 1;
  bool capture;
  GUID name;
  PCFILTER_DESCRIPTOR descriptor{};
  PCPIN_DESCRIPTOR pins[2]{};
  KSDATARANGE analog{};
  PKSDATARANGE ranges[1]{};
  PCCONNECTION_DESCRIPTOR connection{};
  static NTSTATUS jack(PPCPROPERTY_REQUEST request) {
    if (request->Verb & KSPROPERTY_TYPE_BASICSUPPORT)
      return property_support(request, KSPROPERTY_TYPE_GET);
    const auto* self = static_cast<Topology*>(request->MajorTarget);
    if (!(request->Verb & KSPROPERTY_TYPE_GET) || request->InstanceSize < sizeof(ULONG) * 2 ||
        !request->Instance || *static_cast<ULONG*>(request->Instance) != (self->capture ? 0u : 1u))
      return STATUS_INVALID_PARAMETER;
    const ULONG size = sizeof(KSMULTIPLE_ITEM) + sizeof(KSJACK_DESCRIPTION);
    if (!request->ValueSize) {
      request->ValueSize = size;
      return STATUS_BUFFER_OVERFLOW;
    }
    if (!request->Value || request->ValueSize < size)
      return STATUS_BUFFER_TOO_SMALL;
    auto* list = static_cast<KSMULTIPLE_ITEM*>(request->Value);
    RtlZeroMemory(list, size);
    list->Size = size;
    list->Count = 1;
    auto* item = reinterpret_cast<KSJACK_DESCRIPTION*>(list + 1);
    item->ChannelMapping = KSAUDIO_SPEAKER_STEREO;
    item->ConnectionType = eConnTypeUnknown;
    item->GeoLocation = eGeoLocNotApplicable;
    item->GenLocation = eGenLocInternal;
    item->PortConnection = ePortConnIntegratedDevice;
    item->IsConnected = TRUE;
    request->ValueSize = size;
    return STATUS_SUCCESS;
  }
  static const PCPROPERTY_ITEM properties[1];
  static const PCAUTOMATION_TABLE automation;

public:
  explicit Topology(bool c, ULONG pair) : capture(c), name(endpoint_name(c, pair)) {
    analog.FormatSize = sizeof(analog);
    analog.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    analog.SubFormat = KSDATAFORMAT_SUBTYPE_ANALOG;
    analog.Specifier = KSDATAFORMAT_SPECIFIER_NONE;
    ranges[0] = &analog;
    for (int i = 0; i < 2; ++i) {
      pins[i].KsPinDescriptor.DataRangesCount = 1;
      pins[i].KsPinDescriptor.DataRanges = ranges;
      pins[i].KsPinDescriptor.DataFlow = i ? KSPIN_DATAFLOW_OUT : KSPIN_DATAFLOW_IN;
      pins[i].KsPinDescriptor.Communication = KSPIN_COMMUNICATION_NONE;
      pins[i].KsPinDescriptor.Category = &KSCATEGORY_AUDIO;
    }
    pins[capture ? 0 : 1].KsPinDescriptor.Category =
        capture ? &KSNODETYPE_MICROPHONE : &KSNODETYPE_SPEAKER;
    pins[capture ? 0 : 1].KsPinDescriptor.Name = &name;
    connection = {PCFILTER_NODE, 0, PCFILTER_NODE, 1};
    descriptor.PinSize = sizeof(PCPIN_DESCRIPTOR);
    descriptor.AutomationTable = &automation;
    descriptor.PinCount = 2;
    descriptor.Pins = pins;
    descriptor.ConnectionCount = 1;
    descriptor.Connections = &connection;
  }
  STDMETHODIMP_(NTSTATUS) QueryInterface(REFIID id, PVOID* out) override {
    if (!out)
      return STATUS_INVALID_PARAMETER;
    *out = nullptr;
    if (IsEqualGUIDAligned(id, IID_IUnknown) || IsEqualGUIDAligned(id, IID_IMiniport) ||
        IsEqualGUIDAligned(id, IID_IMiniportTopology)) {
      *out = static_cast<IMiniportTopology*>(this);
      AddRef();
      return STATUS_SUCCESS;
    }
    return STATUS_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override {
    return InterlockedIncrement(&refs);
  }
  STDMETHODIMP_(ULONG) Release() override {
    auto n = InterlockedDecrement(&refs);
    if (!n)
      delete this;
    return n;
  }
  STDMETHODIMP_(NTSTATUS) GetDescription(PPCFILTER_DESCRIPTOR* out) override {
    *out = &descriptor;
    return STATUS_SUCCESS;
  }
  STDMETHODIMP_(NTSTATUS)
  DataRangeIntersection(ULONG, PKSDATARANGE, PKSDATARANGE, ULONG, PVOID, PULONG) override {
    return STATUS_NOT_SUPPORTED;
  }
  STDMETHODIMP_(NTSTATUS) Init(PUNKNOWN, PRESOURCELIST, PPORTTOPOLOGY) override {
    return STATUS_SUCCESS;
  }
};
const PCPROPERTY_ITEM Topology::properties[1] = {
    {&KSPROPSETID_Jack, KSPROPERTY_JACK_DESCRIPTION,
     KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT, Topology::jack}};
const PCAUTOMATION_TABLE Topology::automation = {
    sizeof(PCPROPERTY_ITEM), 1, Topology::properties, 0, 0, nullptr, 0, 0, nullptr, 0};
static NTSTATUS register_part(PDEVICE_OBJECT device, PIRP irp, PRESOURCELIST resources, PWSTR name,
                              bool capture, bool topology, ULONG pair, PPORT* result) {
  PPORT port = nullptr;
  auto status = PcNewPort(&port, topology ? CLSID_PortTopology : CLSID_PortWaveRT);
  if (!NT_SUCCESS(status))
    return status;
  PUNKNOWN mini = topology ? static_cast<PUNKNOWN>(new Topology(capture, pair))
                           : static_cast<PUNKNOWN>(new Wave(capture, pair));
  if (!mini) {
    port->Release();
    return STATUS_INSUFFICIENT_RESOURCES;
  }
  status = port->Init(device, irp, mini, nullptr, resources);
  mini->Release();
  if (NT_SUCCESS(status))
    status = PcRegisterSubdevice(device, name, port);
  if (!NT_SUCCESS(status)) {
    port->Release();
    return status;
  }
  *result = port;
  return STATUS_SUCCESS;
}
static void part_name(WCHAR* text, bool capture, bool topology, ULONG pair) {
  const WCHAR* prefix = topology ? (capture ? L"TopologyCapture" : L"TopologyRender")
                                 : (capture ? L"WaveCapture" : L"WaveRender");
  ULONG at = 0;
  while (*prefix)
    text[at++] = *prefix++;
  text[at++] = static_cast<WCHAR>(L'0' + pair / 10);
  text[at++] = static_cast<WCHAR>(L'0' + pair % 10);
  text[at] = 0;
}
static NTSTATUS NTAPI start_device(PDEVICE_OBJECT device, PIRP irp, PRESOURCELIST resources) {
  for (ULONG pair = 0; pair < OPENAOIP_WDM_PAIRS; ++pair) {
    PPORT wave_render = nullptr, topo_render = nullptr, wave_capture = nullptr,
          topo_capture = nullptr;
    WCHAR name[32]{};
    part_name(name, false, false, pair);
    auto status = register_part(device, irp, resources, name, false, false, pair, &wave_render);
    if (NT_SUCCESS(status)) {
      part_name(name, false, true, pair);
      status = register_part(device, irp, resources, name, false, true, pair, &topo_render);
    }
    if (NT_SUCCESS(status)) {
      part_name(name, true, false, pair);
      status = register_part(device, irp, resources, name, true, false, pair, &wave_capture);
    }
    if (NT_SUCCESS(status)) {
      part_name(name, true, true, pair);
      status = register_part(device, irp, resources, name, true, true, pair, &topo_capture);
    }
    if (NT_SUCCESS(status))
      status = PcRegisterPhysicalConnection(device, wave_render, 1, topo_render, 0);
    if (NT_SUCCESS(status))
      status = PcRegisterPhysicalConnection(device, topo_capture, 1, wave_capture, 0);
    if (wave_render)
      wave_render->Release();
    if (topo_render)
      topo_render->Release();
    if (wave_capture)
      wave_capture->Release();
    if (topo_capture)
      topo_capture->Release();
    if (!NT_SUCCESS(status))
      return status;
  }
  return STATUS_SUCCESS;
}
static NTSTATUS NTAPI add_device(PDRIVER_OBJECT driver, PDEVICE_OBJECT physical) {
  return PcAddAdapterDevice(driver, physical, start_device, OPENAOIP_WDM_PAIRS * 4, 0);
}
static void clear_queues() {
  for (ULONG i = 0; i < OPENAOIP_WDM_PAIRS; ++i) {
    render_queue[i].clear();
    capture_queue[i].clear();
  }
}
static NTSTATUS NTAPI dispatch(PDEVICE_OBJECT device, PIRP irp) {
  const auto* stack = IoGetCurrentIrpStackLocation(irp);
  if (device != control_device)
    return previous[stack->MajorFunction](device, irp);
  NTSTATUS result = STATUS_SUCCESS;
  ULONG_PTR size = 0;
  if (stack->MajorFunction == IRP_MJ_CLEANUP) {
    KIRQL level;
    KeAcquireSpinLock(&queue_lock, &level);
    if (owner_file == stack->FileObject) {
      owner_file = nullptr;
      owner_pid = 0;
      clear_queues();
    }
    KeReleaseSpinLock(&queue_lock, level);
  } else if (stack->MajorFunction == IRP_MJ_DEVICE_CONTROL) {
    const auto code = stack->Parameters.DeviceIoControl.IoControlCode;
    const auto input = stack->Parameters.DeviceIoControl.InputBufferLength,
               output = stack->Parameters.DeviceIoControl.OutputBufferLength;
    auto* buffer = irp->AssociatedIrp.SystemBuffer;
    KIRQL level;
    KeAcquireSpinLock(&queue_lock, &level);
    if (code == OPENAOIP_IOCTL_STATUS && output >= sizeof(OPENAOIP_WDM_STATUS) && buffer) {
      auto* s = static_cast<OPENAOIP_WDM_STATUS*>(buffer);
      RtlZeroMemory(s, sizeof(*s));
      s->config = configuration;
      s->render_mask = render_mask;
      s->capture_mask = capture_mask;
      s->owner_pid = owner_pid;
      s->pair_count = OPENAOIP_WDM_PAIRS;
      for (ULONG i = 0; i < OPENAOIP_WDM_PAIRS; ++i) {
        s->render_frames += render_queue[i].frames;
        s->capture_frames += capture_queue[i].frames;
        s->render_dropped += render_queue[i].dropped;
        s->capture_underruns += capture_queue[i].underruns;
      }
      size = sizeof(*s);
    } else if (code == OPENAOIP_IOCTL_CONFIGURE && input == sizeof(OPENAOIP_WDM_CONFIG) && buffer) {
      const auto requested = *static_cast<OPENAOIP_WDM_CONFIG*>(buffer);
      if (!aoip_wdm::valid_config(requested.version, requested.tx_channels, requested.rx_channels,
                                  requested.sample_rate))
        result = STATUS_INVALID_PARAMETER;
      else if (owner_file && owner_file != stack->FileObject)
        result = STATUS_SHARING_VIOLATION;
      else {
        configuration = requested;
        owner_file = stack->FileObject;
        owner_pid = IoGetRequestorProcessId(irp);
        clear_queues();
      }
    } else if (owner_file != stack->FileObject || owner_pid != IoGetRequestorProcessId(irp))
      result = STATUS_ACCESS_DENIED;
    else if ((code == OPENAOIP_IOCTL_READ_RENDER || code == OPENAOIP_IOCTL_WRITE_CAPTURE) &&
             input == sizeof(OPENAOIP_WDM_BLOCK) && buffer) {
      const auto* request = static_cast<const OPENAOIP_WDM_BLOCK*>(buffer);
      const auto pair = request->pair, frames = request->frames;
      if (!aoip_wdm::valid_block(request->version, pair, frames))
        result = STATUS_INVALID_PARAMETER;
      else if (code == OPENAOIP_IOCTL_READ_RENDER && output >= sizeof(OPENAOIP_WDM_BLOCK)) {
        auto* reply = static_cast<OPENAOIP_WDM_BLOCK*>(buffer);
        RtlZeroMemory(reply, sizeof(*reply));
        reply->version = OPENAOIP_WDM_VERSION;
        reply->pair = pair;
        reply->running = (render_mask >> pair) & 1;
        reply->frames = render_queue[pair].pop(reply->samples, frames, false);
        size = sizeof(*reply);
      } else if (code == OPENAOIP_IOCTL_WRITE_CAPTURE) {
        if ((capture_mask >> pair) & 1)
          capture_queue[pair].push(request->samples, frames);
      } else
        result = STATUS_INVALID_PARAMETER;
    } else
      result = STATUS_INVALID_PARAMETER;
    KeReleaseSpinLock(&queue_lock, level);
  } else if (stack->MajorFunction != IRP_MJ_CREATE && stack->MajorFunction != IRP_MJ_CLOSE)
    result = STATUS_INVALID_DEVICE_REQUEST;
  irp->IoStatus.Status = result;
  irp->IoStatus.Information = size;
  IoCompleteRequest(irp, IO_NO_INCREMENT);
  return result;
}
static void NTAPI unload(PDRIVER_OBJECT driver) {
  UNICODE_STRING link = RTL_CONSTANT_STRING(L"\\DosDevices\\OpenAoIPWdm");
  IoDeleteSymbolicLink(&link);
  if (control_device)
    IoDeleteDevice(control_device);
  if (previous_unload)
    previous_unload(driver);
}
extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver, PUNICODE_STRING path) {
  KeInitializeSpinLock(&queue_lock);
  auto status = PcInitializeAdapterDriver(driver, path, add_device);
  if (!NT_SUCCESS(status))
    return status;
  for (ULONG i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; ++i)
    previous[i] = driver->MajorFunction[i];
  previous_unload = driver->DriverUnload;
  driver->DriverUnload = unload;
  UNICODE_STRING name = RTL_CONSTANT_STRING(L"\\Device\\OpenAoIPWdm"),
                 link = RTL_CONSTANT_STRING(L"\\DosDevices\\OpenAoIPWdm");
  UNICODE_STRING security = RTL_CONSTANT_STRING(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)");
  const GUID security_class = {
      0x04a3671d, 0x50e9, 0x430f, {0xae, 0x37, 0x12, 0x4e, 0xe2, 0x8b, 0x90, 0x10}};
  status = IoCreateDeviceSecure(driver, 0, &name, FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN,
                                FALSE, &security, &security_class, &control_device);
  if (!NT_SUCCESS(status))
    return status;
  status = IoCreateSymbolicLink(&link, &name);
  if (!NT_SUCCESS(status)) {
    IoDeleteDevice(control_device);
    control_device = nullptr;
    return status;
  }
  control_device->Flags |= DO_BUFFERED_IO;
  control_device->Flags &= ~DO_DEVICE_INITIALIZING;
  for (ULONG i = 0; i <= IRP_MJ_MAXIMUM_FUNCTION; ++i)
    driver->MajorFunction[i] = dispatch;
  return STATUS_SUCCESS;
}
