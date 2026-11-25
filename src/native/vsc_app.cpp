#include "aoip/discovery.hpp"
#include "aoip/engine_host.hpp"
#include "aoip/network.hpp"
#include "aoip/vsc_engine.hpp"
#include "aoip/wdm_client.hpp"
#include "app_window.hpp"
#include <commdlg.h>
#include <shellapi.h>
namespace {
using namespace aoip;
using namespace aoip::ui;
enum Id {
  Interface = 101,
  ControllerAddress,
  TxChannels,
  RxChannels,
  SampleRate,
  Latency,
  Clock,
  Api,
  Start,
  Scan,
  Register,
  DeviceName,
  WindowsSettings,
  Buffer,
  OpenBridge
};
class SoundcardWindow final : public AppWindow {
public:
  vsc::Settings settings;
  std::filesystem::path folder, ipc;
  std::unique_ptr<engine::Client> client;
  Json live = Json::object(), wdm = Json::object();
  bool active = false;
  ULONGLONG probed = 0;
  const std::vector<int> channel_values{1, 2, 4, 8, 16, 32}, rate_values{44100, 48000, 96000},
      latency_values{20, 40, 60, 100, 200, 500}, buffer_values{64, 128, 256, 512, 1024};
  std::vector<std::string> interfaces{"0.0.0.0"};
  bool windows_mode() const {
    return controls.contains(Api) ? selection(Api) == 1 : settings.backend == "wdm";
  }
  bool asio_registered() const {
    HKEY key = nullptr;
    const auto result =
        RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO\\OpenAoIP Virtual Soundcard", 0,
                      KEY_READ | KEY_WOW64_64KEY, &key);
    if (key)
      RegCloseKey(key);
    if (result != ERROR_SUCCESS)
      return false;
    wchar_t path[32768]{};
    DWORD bytes = sizeof(path);
    if (RegGetValueW(HKEY_CLASSES_ROOT,
                     L"CLSID\\{40E619D1-9FA6-4A49-9317-86FA9E6A3D2B}\\InprocServer32", nullptr,
                     RRF_RT_REG_SZ, nullptr, path, &bytes) != ERROR_SUCCESS)
      return false;
    return _wcsicmp(path, (executable_directory() / L"OpenAoIPASIO64.dll").c_str()) == 0;
  }
  void value(int id, const std::vector<int>& values, int n) {
    const auto at = std::find(values.begin(), values.end(), n);
    select(id, at == values.end() ? 0 : static_cast<int>(at - values.begin()));
  }
  void created() override {
    if (!preview) {
      if (folder.empty())
        folder = data_directory(L"VirtualSoundcard");
      const auto file = folder / L"settings.json";
      if (std::filesystem::exists(file))
        settings = vsc::from_json(read_configuration(file));
      else {
        settings.id = "vsc-" + random_id().substr(0, 12);
        wchar_t name[128]{};
        DWORD n = 128;
        GetComputerNameW(name, &n);
        settings.name = utf8(name) + " Virtual Soundcard";
      }
    }
    add(DeviceName, L"EDIT", wide(settings.name), WS_BORDER | ES_AUTOHSCROLL);
    add(ControllerAddress, L"EDIT",
        settings.controller == "auto" ? L"Auto discovery" : wide(settings.controller),
        WS_BORDER | ES_AUTOHSCROLL);
    std::vector<std::wstring> options{L"Auto / all IPv4 interfaces"};
    for (const auto& ip : local_addresses()) {
      interfaces.push_back(ip);
      options.push_back(wide(ip));
    }
    add_combo(Interface, options);
    const auto ip = std::find(interfaces.begin(), interfaces.end(), settings.interface_ip);
    select(Interface, ip == interfaces.end() ? 0 : static_cast<int>(ip - interfaces.begin()));
    add_combo(Api, {L"ASIO x64", L"WDM / Windows audio (experimental)"},
              settings.backend == "wdm" ? 1 : 0);
    std::vector<std::wstring> counts, rates, latencies, buffers;
    for (int n : channel_values)
      counts.push_back(std::to_wstring(n));
    for (int n : rate_values)
      rates.push_back(std::to_wstring(n) + L" Hz");
    for (int n : latency_values)
      latencies.push_back(std::to_wstring(n) + L" ms");
    for (int n : buffer_values)
      buffers.push_back(std::to_wstring(n) + L" frames");
    add_combo(TxChannels, counts);
    add_combo(RxChannels, counts);
    add_combo(SampleRate, rates);
    add_combo(Latency, latencies);
    add_combo(Buffer, buffers);
    add_combo(Clock, {L"Internal / free running", L"PTPv2 · not implemented"});
    value(TxChannels, channel_values, settings.tx_channels);
    value(RxChannels, channel_values, settings.rx_channels);
    value(SampleRate, rate_values, settings.rate);
    value(Latency, latency_values, settings.latency_ms);
    value(Buffer, buffer_values, settings.buffer_frames);
    add_button(Start, L"Start");
    add_button(Scan, L"Find");
    add_button(Register, L"Register ASIO");
    add_button(WindowsSettings, L"Windows sound");
    add_button(OpenBridge, L"Open Audio Bridge");
    if (!preview) {
      client = std::make_unique<engine::Client>(executable_directory() / L"OpenAoIP Engine.exe",
                                                ipc.empty() ? engine_endpoint_file() : ipc);
      wdm = vsc::wdm_probe();
      try {
        live = client->status(settings.id);
        active = live.value("active", false);
      } catch (const std::exception& e) {
        message = wide(e.what());
        message_color = red;
      }
    }
    update_controls();
  }
  void layout() override {
    if (controls.empty())
      return;
    const int form = std::max(360, width * 53 / 100), field = form - 148, half = (field - 10) / 2;
    position(Api, {148, 108, field, 28});
    position(DeviceName, {148, 148, field, 28});
    position(Interface, {148, 188, field, 28});
    position(ControllerAddress, {148, 228, field - 62, 28});
    position(Scan, {form - 54, 228, 54, 28});
    position(TxChannels, {148, 284, half, 28});
    position(RxChannels, {158 + half, 284, half, 28});
    position(SampleRate, {148, 340, half, 28});
    position(Buffer, {158 + half, 340, half, 28});
    position(Latency, {148, 396, half, 28});
    position(Clock, {158 + half, 396, half, 28});
    position(Start, {12, 65, 105, 28});
    position(Register, {125, 65, 150, 28});
    position(WindowsSettings, {283, 65, 139, 28});
    position(OpenBridge, {width - 170, 65, 158, 28});
  }
  void update_controls() {
    if (controls.empty())
      return;
    for (auto& [id, child] : controls)
      if (id != Start && id != Register && id != WindowsSettings && id != OpenBridge)
        EnableWindow(child, !active);
    if (windows_mode())
      value(SampleRate, rate_values, 48000);
    EnableWindow(controls[SampleRate], !active && !windows_mode());
    EnableWindow(controls[Buffer], !active && !windows_mode());
    set_text(controls[Start], active ? L"Stop" : L"Start");
    set_text(controls[Register], windows_mode() ? L"Install WDM driver" : L"Register ASIO");
    EnableWindow(controls[Register], !active && (windows_mode() || !asio_registered()));
  }
  void read() {
    settings.name = utf8(get_text(controls[DeviceName]));
    settings.interface_ip = interfaces.at(selection(Interface));
    const auto address = utf8(get_text(controls[ControllerAddress]));
    settings.controller = address.empty() || address == "Auto discovery" ? "auto" : address;
    settings.backend = windows_mode() ? "wdm" : "asio";
    settings.tx_channels = channel_values.at(selection(TxChannels));
    settings.rx_channels = channel_values.at(selection(RxChannels));
    settings.rate = rate_values.at(selection(SampleRate));
    settings.buffer_frames = buffer_values.at(selection(Buffer));
    settings.latency_ms = latency_values.at(selection(Latency));
    settings.clock_source = selection(Clock) == 0 ? "internal" : "ptp";
  }
  void timer() override {
    if (preview)
      return;
    if (GetTickCount64() - probed > 3000) {
      wdm = vsc::wdm_probe();
      probed = GetTickCount64();
    }
    if (client) {
      try {
        live = client->status(settings.id);
        active = live.value("active", false);
        const auto error = live.value("error", std::string{}),
                   network = live.value("networkError", std::string{});
        if (!error.empty()) {
          message = wide(error);
          message_color = red;
        } else if (!network.empty()) {
          message = wide(network);
          message_color = amber;
        } else {
          message = active ? L"Audio engine running in this Windows user session. ASIO and WDM "
                             L"share one session owner."
                           : L"Ready · ASIO needs no Windows test mode. WDM requires an installed "
                             L"compatible driver.";
          message_color = muted;
        }
      } catch (const std::exception& e) {
        message = wide(e.what());
        message_color = red;
      }
    }
    update_controls();
  }
  void command(int id, int notice) override {
    if (id == Api && notice == CBN_SELCHANGE) {
      update_controls();
      return;
    }
    if (id == OpenBridge) {
      const auto file = executable_directory() / L"OpenAoIP Audio Bridge.exe";
      if (reinterpret_cast<INT_PTR>(
              ShellExecuteW(window, L"open", file.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        throw std::runtime_error("Install OpenAoIP Audio Bridge to use local WASAPI devices");
      return;
    }
    if (id == WindowsSettings) {
      ShellExecuteW(window, L"open", L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL);
      return;
    }
    if (preview)
      return;
    if (id == Start) {
      if (active) {
        client->stop(settings.id);
        active = false;
        live = Json::object();
      } else {
        read();
        if (windows_mode() &&
            (!wdm.value("available", false) || wdm.value("windowsRenderEndpoints", 0) < 16 ||
             wdm.value("windowsCaptureEndpoints", 0) < 16))
          throw std::runtime_error("WDM is unavailable: Windows must accept a compatible driver "
                                   "and expose its audio endpoints first");
        live = client->start("vsc", vsc::to_json(settings));
        active = true;
        write_configuration(folder / L"settings.json", vsc::to_json(settings));
      }
      update_controls();
      timer();
    }
    if (id == Scan) {
      const auto found = discover_controllers(interfaces.at(selection(Interface)), 500);
      if (found.empty())
        throw std::runtime_error("No controller found; check the LAN or enter its http:// address");
      set_text(controls[ControllerAddress], wide(found.front().url));
    }
    if (id == Register) {
      if (windows_mode()) {
        wchar_t file[32768] = L"OpenAoIPWdm.inf";
        OPENFILENAMEW open{sizeof(open)};
        open.hwndOwner = window;
        open.lpstrFilter = L"Signed OpenAoIP driver\0OpenAoIPWdm.inf\0\0";
        open.lpstrFile = file;
        open.nMaxFile = 32768;
        open.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (!GetOpenFileNameW(&open))
          return;
        const auto tool = executable_directory() / L"OpenAoIPWdmSetup.exe";
        const auto args = L"--install \"" + std::wstring(file) + L"\"";
        ShellExecuteW(window, L"runas", tool.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
      } else {
        wchar_t system[MAX_PATH]{};
        GetSystemDirectoryW(system, MAX_PATH);
        const auto tool = std::filesystem::path(system) / L"regsvr32.exe",
                   driver = executable_directory() / L"OpenAoIPASIO64.dll";
        if (!std::filesystem::exists(driver))
          throw std::runtime_error("ASIO DLL is missing beside the soundcard application");
        const auto args = L"\"" + driver.wstring() + L"\"";
        ShellExecuteW(window, L"runas", tool.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
      }
    }
  }
  bool close() override {
    if (!preview && !active)
      try {
        read();
        write_configuration(folder / L"settings.json", vsc::to_json(settings));
      } catch (...) {
      }
    if (active && client)
      try {
        client->stop(settings.id);
      } catch (...) {
      }
    if (client)
      try {
        client->release_if_idle();
      } catch (...) {
      }
    return true;
  }
  void paint(HDC dc) override {
    const bool run = active && live.value("running", false),
               connected = live.value("controllerConnected", false);
    header(dc, L"OpenAoIP Virtual Soundcard",
           run      ? L"Running"
           : active ? L"Error"
                    : L"Stopped",
           run      ? green
           : active ? red
                    : gray);
    const int form = std::max(360, width * 53 / 100), half = (form - 158) / 2;
    line(dc, form + 20, 108, form + 20, height - 43, border);
    for (const auto& label : std::vector<std::pair<int, std::wstring>>{{108, L"Audio interface"},
                                                                       {148, L"Device name"},
                                                                       {188, L"Network interface"},
                                                                       {228, L"Controller"}})
      text(dc, {12, label.first, 130, 28}, label.second, 12, muted);
    text(dc, {148, 263, half, 18}, L"TX / application outputs", 10, muted);
    text(dc, {158 + half, 263, half, 18}, L"RX / application inputs", 10, muted);
    text(dc, {148, 319, half, 18}, L"Sample rate", 10, muted);
    text(dc, {158 + half, 319, half, 18},
         windows_mode() ? L"Buffer: Windows controlled" : L"Preferred ASIO buffer", 10, muted);
    text(dc, {148, 375, half, 18}, L"Network latency", 10, muted);
    text(dc, {158 + half, 375, half, 18}, L"Clock source", 10, muted);
    const int x = form + 38;
    int y = 108;
    text(dc, {x, y, width - x - 14, 26}, L"STATUS / DIAGNOSTICS", 11, ink, FW_SEMIBOLD);
    y += 34;
    const int rate = live.value("sampleRate", settings.rate);
    const bool locked = live.value("synchronized", false);
    std::vector<std::pair<std::wstring, std::wstring>> rows = {
        {L"Network", connected ? L"Connected"
                     : active  ? L"Waiting for controller"
                               : L"Not connected"},
        {L"Clock", locked ? L"Locked" : L"Free running · not PTP locked"},
        {L"Sample rate", std::to_wstring(rate) + L" Hz"},
        {L"Active TX / RX",
         std::to_wstring(active ? live.value("txChannels", settings.tx_channels) : 0) + L" / " +
             std::to_wstring(active ? live.value("rxChannels", settings.rx_channels) : 0)},
        {L"RTP bandwidth",
         std::to_wstring(static_cast<int>(
             (live.value("txBitsPerSecond", 0.) + live.value("rxBitsPerSecond", 0.)) / 1000)) +
             L" kbit/s"},
        {L"Packet loss / late", std::to_wstring(live.value("packetLoss", 0ULL)) + L" / " +
                                    std::to_wstring(live.value("latePackets", 0ULL))},
        {L"Buffer underruns", std::to_wstring(live.value("bufferUnderruns", 0ULL))},
        {L"Driver", windows_mode()
                        ? (wdm.value("available", false) ? L"WDM connected · experimental"
                                                         : L"WDM not installed / not accepted")
                        : (asio_registered() ? L"ASIO registered" : L"ASIO not registered")},
        {L"Host buffer", live.value("activeBufferFrames", 0) > 0
                             ? std::to_wstring(live.value("activeBufferFrames", 0)) + L" frames"
                             : L"No ASIO buffers open"}};
    for (const auto& row : rows) {
      text(dc, {x, y, 118, 26}, row.first, 11, muted);
      text(dc, {x + 120, y, width - x - 132, 26}, row.second, 11,
           row.first == L"Clock"     ? locked ? green : gray
           : row.first == L"Network" ? connected ? green : gray
                                     : ink);
      y += 28;
    }
    if (height > 530) {
      const int base = 448;
      line(dc, 12, base - 12, form + 2, base - 12, border);
      text(dc, {12, base - 5, 100, 22}, L"LIVE AUDIO", 10, muted);
      const auto tx = live.value("txPeaks", Json::array()),
                 rx = live.value("rxPeaks", Json::array());
      for (int i = 0; i < 2; ++i) {
        text(dc, {12, base + 22 + i * 29, 95, 22}, L"TX " + std::to_wstring(i + 1), 11);
        meter(dc, {110, base + 27 + i * 29, form - 118, 10},
              i < int(tx.size()) ? tx[i].get<float>() : 0.f);
      }
      text(dc, {12, base + 82, form - 12, 38},
           L"ASIO: host chooses the active buffer. WDM: fixed stereo pairs at 48 kHz.\nPTP is not "
           L"available; no Clock Locked state is fabricated.",
           10, muted, FW_NORMAL, DT_WORDBREAK);
    }
  }
};
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  Apartment apartment;
  enable_dpi();
  try {
    NetworkRuntime network;
    SoundcardWindow app;
    std::filesystem::path preview;
    const auto args = arguments();
    for (std::size_t i = 0; i < args.size(); ++i) {
      if (args[i] == L"--data-dir" && i + 1 < args.size())
        app.folder = args[++i];
      else if (args[i] == L"--engine-endpoint" && i + 1 < args.size())
        app.ipc = args[++i];
      else if (args[i] == L"--preview" && i + 1 < args.size())
        preview = args[++i];
      else
        throw std::runtime_error("Unknown soundcard argument");
    }
    return app.run(instance, L"OpenAoIPVirtualSoundcardNative", L"OpenAoIP Virtual Soundcard", 900,
                   610, show, preview);
  } catch (const std::exception& e) {
    MessageBoxW(nullptr, wide(e.what()).c_str(), L"OpenAoIP Virtual Soundcard",
                MB_OK | MB_ICONERROR);
    return 1;
  }
}
