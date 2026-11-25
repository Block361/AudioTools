// SPDX-License-Identifier: MIT
#include "aoip/bridge_engine.hpp"
#include "aoip/engine_host.hpp"
#include "aoip/network.hpp"
#include "app_window.hpp"
#include "clock_view.hpp"
#include <cmath>
#include <sstream>

namespace {
using namespace aoip;
using namespace aoip::ui;
enum EditorId { EName = 301, EMode, EDevice, ELocal, ENetwork, EGain, ESave, ECancel };

std::wstring numbers(const std::vector<bridge::Patch>& patches, bool local) {
  std::wstring value;
  for (const auto& p : patches) {
    if (!value.empty())
      value += L", ";
    value += std::to_wstring(local ? p.local : p.network);
  }
  return value;
}
std::vector<int> channels(const std::wstring& value) {
  std::vector<int> result;
  std::wistringstream input(value);
  std::wstring part;
  while (std::getline(input, part, L',')) {
    std::size_t end = 0;
    const int n = std::stoi(part, &end);
    while (end < part.size() && iswspace(part[end]))
      ++end;
    if (end != part.size() || n < 1 || n > 32)
      throw std::runtime_error("Enter comma-separated channel numbers between 1 and 32");
    result.push_back(n);
  }
  if (result.empty() || result.size() > 32)
    throw std::runtime_error("Assign between 1 and 32 channels");
  return result;
}
class EndpointEditor final : public AppWindow {
public:
  bridge::Endpoint value;
  Json inventory;
  bool saved = false;
  std::vector<std::string> devices;
  EndpointEditor(HWND parent, bridge::Endpoint endpoint, Json list)
      : value(std::move(endpoint)), inventory(std::move(list)) {
    owner = parent;
    modal = true;
  }
  void device_options(bool preserve) {
    const auto selected = preserve ? value.device : std::string("default");
    const bool capture = selection(EMode) == 0;
    SendMessageW(controls[EDevice], CB_RESETCONTENT, 0, 0);
    devices = {"default"};
    SendMessageW(controls[EDevice], CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"Windows default device"));
    int current = 0;
    for (const auto& d : inventory.value(capture ? "capture" : "playback", Json::array())) {
      devices.push_back(d.at("id"));
      const auto label = wide(d.at("name"));
      SendMessageW(controls[EDevice], CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
      if (devices.back() == selected)
        current = static_cast<int>(devices.size()) - 1;
    }
    if (current == 0 && selected != "default") {
      devices.push_back(selected);
      const auto label = L"Unavailable / saved device: " + wide(selected);
      SendMessageW(controls[EDevice], CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
      current = static_cast<int>(devices.size()) - 1;
    }
    select(EDevice, current);
  }
  void created() override {
    add(EName, L"EDIT", wide(value.name), WS_BORDER | ES_AUTOHSCROLL);
    add_combo(EMode,
              {L"WASAPI capture → network TX", L"WASAPI loopback → network TX",
               L"Network RX → WASAPI playback"},
              value.mode == EndpointMode::Capture    ? 0
              : value.mode == EndpointMode::Loopback ? 1
                                                     : 2);
    add_combo(EDevice, {});
    device_options(true);
    add(ELocal, L"EDIT", numbers(value.patches, true), WS_BORDER | ES_AUTOHSCROLL);
    add(ENetwork, L"EDIT", numbers(value.patches, false), WS_BORDER | ES_AUTOHSCROLL);
    add(EGain, L"EDIT",
        value.patches.empty() ? L"1.0" : std::to_wstring(value.patches.front().gain),
        WS_BORDER | ES_AUTOHSCROLL);
    add_button(ESave, L"Apply");
    add_button(ECancel, L"Cancel");
    message = L"Local and network lists must contain the same number of channels.";
  }
  void layout() override {
    if (controls.empty())
      return;
    for (const auto& row : std::vector<std::pair<int, int>>{{EName, 106},
                                                            {EMode, 146},
                                                            {EDevice, 186},
                                                            {ELocal, 254},
                                                            {ENetwork, 294},
                                                            {EGain, 334}})
      position(row.first, {180, row.second, width - 196, 28});
    position(ESave, {width - 212, height - 74, 92, 29});
    position(ECancel, {width - 108, height - 74, 92, 29});
  }
  void command(int id, int notice) override {
    if (id == EMode && notice == CBN_SELCHANGE) {
      device_options(false);
      return;
    }
    if (id == ECancel) {
      DestroyWindow(window);
      return;
    }
    if (id != ESave)
      return;
    const auto local = channels(get_text(controls[ELocal])),
               net = channels(get_text(controls[ENetwork]));
    if (local.size() != net.size())
      throw std::runtime_error("Local and network lists must have the same length");
    const auto raw = get_text(controls[EGain]);
    std::size_t end = 0;
    const float gain = std::stof(raw, &end);
    while (end < raw.size() && iswspace(raw[end]))
      ++end;
    if (end != raw.size() || !std::isfinite(gain) || gain < 0 || gain > 2)
      throw std::runtime_error("Gain must be between 0.0 and 2.0");
    value.name = utf8(get_text(controls[EName]));
    if (value.name.empty() || value.name.size() > 160)
      throw std::runtime_error("Enter a device label of at most 160 UTF-8 bytes");
    value.mode = selection(EMode) == 0   ? EndpointMode::Capture
                 : selection(EMode) == 1 ? EndpointMode::Loopback
                                         : EndpointMode::Playback;
    value.device = devices.at(selection(EDevice));
    value.patches.clear();
    for (std::size_t i = 0; i < local.size(); ++i)
      value.patches.push_back({local[i], net[i], gain});
    saved = true;
    DestroyWindow(window);
  }
  void paint(HDC dc) override {
    header(dc, L"Audio Bridge · Local device");
    for (const auto& row : std::vector<std::pair<int, std::wstring>>{{106, L"Label"},
                                                                     {146, L"Direction"},
                                                                     {186, L"Windows device"},
                                                                     {254, L"Local channels"},
                                                                     {294, L"Network channels"},
                                                                     {334, L"Gain (linear)"}})
      text(dc, {16, row.first, 156, 28}, row.second, 12, muted);
    text(dc, {180, 222, width - 196, 24},
         selection(EMode) == 1
             ? L"Loopback captures audio played through the selected Windows output."
             : L"Example stereo map: local 1, 2 → network 3, 4. Mono: 1 → 1.",
         11, muted);
    text(dc, {16, 379, width - 32, 48},
         L"Capture/loopback feed TX. Playback consumes RX. Use one entry per device and direction; "
         L"add all its channel assignments here.",
         11, muted, FW_NORMAL, DT_WORDBREAK);
  }
};

enum Id {
  Start = 109,
  Add = 401,
  Edit,
  Remove,
  Scan,
  Save,
  Load,
  Name,
  Interface,
  ControllerAddress,
  Tx,
  Rx,
  Rate,
  Buffer,
  NetLatency,
  LocalLatency,
  Clock,
  VScroll,
  PtpDomain
};
class BridgeWindow final : public AppWindow {
public:
  bridge::Settings settings;
  std::filesystem::path folder, config_file, ipc;
  std::unique_ptr<engine::Client> client;
  Json live = Json::object(), inventory = Json::object();
  bool active = false;
  int chosen = -1, tab = 0, offset = 0;
  const std::vector<int> counts{1, 2, 4, 8, 16, 32}, rates{44100, 48000, 96000},
      buffers{64, 128, 256, 512, 1024, 2048}, net_latencies{20, 40, 60, 100, 200, 500},
      local_latencies{5, 10, 20, 40, 80, 100, 200};
  std::vector<std::string> interfaces{"0.0.0.0"};
  void choose(int id, const std::vector<int>& values, int value) {
    const auto it = std::find(values.begin(), values.end(), value);
    select(id, it == values.end() ? 0 : static_cast<int>(it - values.begin()));
  }
  void add_numbers(int id, const std::vector<int>& values) {
    std::vector<std::wstring> labels;
    for (int n : values)
      labels.push_back(std::to_wstring(n));
    add_combo(id, labels);
  }
  void populate() {
    set_text(controls[Name], wide(settings.name));
    set_text(controls[ControllerAddress],
             settings.controller == "auto" ? L"Auto discovery" : wide(settings.controller));
    const auto ip = std::find(interfaces.begin(), interfaces.end(), settings.interface_ip);
    select(Interface, ip == interfaces.end() ? 0 : static_cast<int>(ip - interfaces.begin()));
    choose(Tx, counts, settings.tx_channels);
    choose(Rx, counts, settings.rx_channels);
    choose(Rate, rates, settings.rate);
    choose(Buffer, buffers, settings.buffer_frames);
    choose(NetLatency, net_latencies, settings.latency_ms);
    choose(LocalLatency, local_latencies, settings.local_latency_ms);
    select(Clock, settings.clock_source == "ptp-monitor" ? 1 : 0);
    set_text(controls[PtpDomain], std::to_wstring(settings.ptp_domain));
  }
  void created() override {
    if (!preview) {
      if (folder.empty())
        folder = data_directory(L"AudioBridge");
      if (config_file.empty())
        config_file = folder / L"settings.json";
      if (std::filesystem::exists(config_file))
        settings = bridge::from_json(read_configuration(config_file));
      else {
        settings.id = "bridge-" + random_id().substr(0, 12);
        wchar_t host[128]{};
        DWORD len = 128;
        GetComputerNameW(host, &len);
        settings.name = utf8(host) + " Audio Bridge";
      }
    }
    for (const auto& b : std::vector<std::pair<int, std::wstring>>{{Start, L"Start"},
                                                                   {Add, L"Add device"},
                                                                   {Edit, L"Edit"},
                                                                   {Remove, L"Remove"},
                                                                   {Scan, L"Scan devices"},
                                                                   {Save, L"Save config"},
                                                                   {Load, L"Load config"}})
      add_button(b.first, b.second);
    add(Name, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL);
    add(ControllerAddress, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL);
    std::vector<std::wstring> ips{L"Auto / all IPv4 interfaces"};
    for (const auto& ip : local_addresses()) {
      interfaces.push_back(ip);
      ips.push_back(wide(ip));
    }
    add_combo(Interface, ips);
    add_numbers(Tx, counts);
    add_numbers(Rx, counts);
    add_numbers(Rate, rates);
    add_numbers(Buffer, buffers);
    add_numbers(NetLatency, net_latencies);
    add_numbers(LocalLatency, local_latencies);
    add_combo(Clock, {L"Internal / drift compensated", L"PTPv2 monitor (experimental)"});
    add(PtpDomain, L"EDIT", L"0", WS_BORDER | ES_NUMBER);
    SendMessageW(controls[PtpDomain], EM_SETLIMITTEXT, 3, 0);
    add(VScroll, L"SCROLLBAR", L"", SBS_VERT);
    populate();
    if (!preview) {
      client = std::make_unique<engine::Client>(executable_directory() / L"OpenAoIP Engine.exe",
                                                ipc.empty() ? engine_endpoint_file() : ipc);
      try {
        live = client->status(settings.id);
        active = live.value("active", false);
        inventory = client->devices();
        if (active && live.contains("config")) {
          settings = bridge::from_json(live.at("config"));
          populate();
        }
      } catch (const std::exception& e) {
        message = wide(e.what());
        message_color = red;
      }
    }
    update_controls();
  }
  void read() {
    settings.name = utf8(get_text(controls[Name]));
    settings.interface_ip = interfaces.at(selection(Interface));
    const auto value = utf8(get_text(controls[ControllerAddress]));
    settings.controller = value.empty() || value == "Auto discovery" ? "auto" : value;
    settings.tx_channels = counts.at(selection(Tx));
    settings.rx_channels = counts.at(selection(Rx));
    settings.rate = rates.at(selection(Rate));
    settings.buffer_frames = buffers.at(selection(Buffer));
    settings.latency_ms = net_latencies.at(selection(NetLatency));
    settings.local_latency_ms = local_latencies.at(selection(LocalLatency));
    settings.clock_source = selection(Clock) == 0 ? "internal" : "ptp-monitor";
    settings.ptp_domain = ptp_domain(controls[PtpDomain]);
  }
  int half() const {
    return width / 2;
  }
  void layout() override {
    if (controls.empty())
      return;
    int x = 12;
    for (const auto& b : std::vector<std::pair<int, int>>{{Start, 88},
                                                          {Add, 100},
                                                          {Edit, 62},
                                                          {Remove, 76},
                                                          {Scan, 103},
                                                          {Save, 100},
                                                          {Load, 100}}) {
      position(b.first, {x, 65, b.second, 28});
      x += b.second + 6;
    }
    const int a = half() - 118, b = (a - 8) / 2;
    position(Name, {104, 108, a, 28});
    position(Interface, {half() + 112, 108, a - 8, 28});
    position(ControllerAddress, {104, 148, a, 28});
    position(Rate, {half() + 112, 148, a - 8, 28});
    position(Tx, {104, 188, b, 28});
    position(Rx, {112 + b, 188, b, 28});
    position(Buffer, {half() + 112, 188, a - 8, 28});
    position(NetLatency, {104, 228, b, 28});
    position(LocalLatency, {112 + b, 228, b, 28});
    position(Clock, {half() + 112, 228, a - 78, 28});
    position(PtpDomain, {width - 74, 228, 60, 28});
    position(VScroll, {width - 25, 338, 14, std::max(1, height - 461)});
    update_scroll();
  }
  void update_scroll() {
    if (!controls.contains(VScroll))
      return;
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
    info.nMax = std::max(0, (tab == 0   ? static_cast<int>(settings.endpoints.size())
                             : tab == 1 ? std::max(settings.tx_channels, settings.rx_channels)
                                        : 0) -
                                1);
    info.nPage = std::max(1, (height - 464) / 29);
    info.nPos = offset;
    offset = SetScrollInfo(controls[VScroll], SB_CTL, &info, TRUE);
    ShowWindow(controls[VScroll], tab < 2 ? SW_SHOW : SW_HIDE);
  }
  void update_controls() {
    for (auto& [id, child] : controls)
      if (id != Start && id != Save && id != Scan && id != VScroll)
        EnableWindow(child, !active);
    EnableWindow(controls[Edit], !active && chosen >= 0 && chosen < int(settings.endpoints.size()));
    EnableWindow(controls[Remove],
                 !active && chosen >= 0 && chosen < int(settings.endpoints.size()));
    EnableWindow(controls[PtpDomain], !active && selection(Clock) == 1);
    set_text(controls[Start], active ? L"Stop" : L"Start");
    update_scroll();
  }
  void timer() override {
    if (preview)
      return;
    try {
      live = client->status(settings.id);
      active = live.value("active", false);
      if (active) {
        message =
            live.value("audioConnected", false)
                ? L"WASAPI active · local devices reconnect automatically after loss."
                : L"Waiting for a local audio device · inspect its error in Sources / Outputs.";
        message_color = live.value("audioConnected", false) ? muted : amber;
        const auto error = live.value("networkError", std::string{});
        if (!error.empty()) {
          message = wide(error);
          message_color = amber;
        }
      } else {
        message = L"Add WASAPI capture, loopback or playback devices, assign channels, then Start.";
        message_color = muted;
      }
    } catch (const std::exception& e) {
      message = wide(e.what());
      message_color = red;
    }
    update_controls();
  }
  void edit_device(bool add) {
    read();
    inventory = client->devices();
    bridge::Endpoint e;
    if (add) {
      e.id = "local-" + random_id().substr(0, 12);
      e.name = "Local audio";
      e.patches = {{1, 1, 1.f}};
    } else
      e = settings.endpoints.at(chosen);
    EndpointEditor editor(window, e, inventory);
    editor.run(GetModuleHandleW(nullptr), L"OpenAoIPBridgeDeviceEditor",
               L"OpenAoIP · Local audio device", 760, 510, SW_SHOWNORMAL);
    if (editor.saved) {
      if (add) {
        settings.endpoints.push_back(editor.value);
        chosen = int(settings.endpoints.size()) - 1;
      } else
        settings.endpoints[chosen] = editor.value;
      write_configuration(config_file, bridge::to_json(settings));
    }
    update_controls();
  }
  void command(int id, int notice) override {
    if (id == Clock && notice == CBN_SELCHANGE) {
      update_controls();
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
        live = client->start("bridge", bridge::to_json(settings));
        active = true;
        write_configuration(config_file, bridge::to_json(settings));
      }
      update_controls();
      timer();
      return;
    }
    if (id == Add && !active)
      edit_device(true);
    if (id == Edit && !active && chosen >= 0)
      edit_device(false);
    if (id == Remove && !active && chosen >= 0) {
      settings.endpoints.erase(settings.endpoints.begin() + chosen);
      chosen = -1;
      read();
      write_configuration(config_file, bridge::to_json(settings));
      update_controls();
    }
    if (id == Scan) {
      inventory = client->devices();
      message = std::to_wstring(inventory.value("capture", Json::array()).size()) + L" capture / " +
                std::to_wstring(inventory.value("playback", Json::array()).size()) +
                L" playback devices found. Loopback uses playback devices.";
      message_color = muted;
    }
    if (id == Save) {
      if (!active)
        read();
      write_configuration(config_file, bridge::to_json(settings));
      if (auto file = json_file_dialog(window, true, L"OpenAoIP-bridge.json"))
        write_configuration(*file, bridge::to_json(settings));
    }
    if (id == Load && !active) {
      if (auto file = json_file_dialog(window, false)) {
        settings = bridge::from_json(read_configuration(*file));
        chosen = -1;
        populate();
        write_configuration(config_file, bridge::to_json(settings));
        update_controls();
      }
    }
  }
  bool close() override {
    if (!preview && !active)
      try {
        read();
        write_configuration(config_file, bridge::to_json(settings));
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
  Json endpoint_status(const std::string& id) const {
    for (const auto& s : live.value("endpoints", Json::array()))
      if (s.at("id") == id)
        return s;
    return Json::object();
  }
  void paint(HDC dc) override {
    const bool connected = live.value("controllerConnected", false),
               audio = live.value("audioConnected", false);
    header(dc, L"OpenAoIP Audio Bridge",
           active ? (audio ? L"Running" : L"Waiting for device") : L"Stopped",
           active ? (audio ? green : amber) : gray);
    for (const auto& row : std::vector<std::pair<int, std::wstring>>{{108, L"Device name"},
                                                                     {148, L"Controller"},
                                                                     {188, L"TX / RX"},
                                                                     {228, L"Latency (ms)"}})
      text(dc, {12, row.first, 88, 28}, row.second, 11, muted);
    for (const auto& row : std::vector<std::pair<int, std::wstring>>{{108, L"Network interface"},
                                                                     {148, L"Sample rate (Hz)"},
                                                                     {188, L"Buffer (frames)"},
                                                                     {228, L"Clock source"}})
      text(dc, {half() + 10, row.first, 98, 28}, row.second, 11, muted);
    const int label_width = (half() - 126) / 2;
    text(dc, {width - 74, 216, 60, 12}, L"PTP domain", 9, muted);
    text(dc, {104, 258, label_width, 14}, L"Network / receive", 9, muted);
    text(dc, {112 + label_width, 258, label_width, 14}, L"Local / drift buffer", 9, muted);
    const std::vector<std::wstring> tabs{L"Sources / Outputs", L"Network channels", L"Diagnostics"};
    for (int i = 0; i < 3; ++i) {
      Box b{12 + i * 166, 281, 160, 29};
      if (tab == i) {
        fill(dc, b, surface);
        line(dc, b.x, b.y + b.h - 1, b.x + b.w, b.y + b.h - 1, teal, 2);
      }
      text(dc, b, tabs[i], 12, tab == i ? ink : muted, FW_NORMAL,
           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    const int bottom = height - 124;
    if (tab == 0) {
      text(dc, {16, 310, 188, 27}, L"Local device / label", 11, muted);
      text(dc, {210, 310, 125, 27}, L"Direction", 11, muted);
      text(dc, {344, 310, 155, 27}, L"Local → network", 11, muted);
      text(dc, {508, 310, width - 545, 27}, L"Status / error", 11, muted);
      line(dc, 12, 337, width - 30, 337, border);
      if (settings.endpoints.empty())
        text(dc, {24, 355, width - 60, 60},
             L"No local devices configured.\nUse Add device to connect a microphone, soundcard, "
             L"virtual cable or system audio.",
             13, muted, FW_NORMAL, DT_WORDBREAK);
      for (int i = offset; i < int(settings.endpoints.size()); ++i) {
        const int y = 338 + (i - offset) * 29;
        if (y + 29 > bottom)
          break;
        const auto& e = settings.endpoints[i];
        const auto s = endpoint_status(e.id);
        if (chosen == i)
          fill(dc, {12, y, width - 42, 28}, selected);
        text(dc, {16, y, 188, 28}, wide(e.name), 12);
        const auto mode = e.mode == EndpointMode::Playback   ? L"RX → Playback"
                          : e.mode == EndpointMode::Loopback ? L"Loopback → TX"
                                                             : L"Capture → TX";
        text(dc, {210, y, 128, 28}, mode, 11, muted);
        text(dc, {344, y, 155, 28}, numbers(e.patches, true) + L" → " + numbers(e.patches, false),
             11);
        const bool ok = s.value("connected", false);
        const auto error = s.value("error", std::string{});
        text(dc, {508, y, width - 545, 28},
             active ? (ok              ? wide(s.value("deviceName", "Connected"))
                       : error.empty() ? L"Opening / reconnecting"
                                       : wide(error))
                    : L"Stopped",
             11, active ? (ok ? green : amber) : gray);
        line(dc, 12, y + 28, width - 30, y + 28, border);
      }
    } else if (tab == 1) {
      text(dc, {16, 310, 36, 27}, L"Ch", 11, muted);
      text(dc, {58, 310, half() - 70, 27}, L"TX · double-click to rename", 11, muted);
      text(dc, {half() + 12, 310, half() - 50, 27}, L"RX · double-click to rename", 11, muted);
      line(dc, 12, 337, width - 30, 337, border);
      const auto tx = live.value("txPeaks", Json::array()),
                 rx = live.value("rxPeaks", Json::array());
      for (int i = offset; i < std::max(settings.tx_channels, settings.rx_channels); ++i) {
        const int y = 338 + (i - offset) * 29;
        if (y + 29 > bottom)
          break;
        text(dc, {16, y, 34, 28}, std::to_wstring(i + 1), 11, muted);
        if (i < settings.tx_channels) {
          text(dc, {58, y, half() - 218, 28},
               i < int(settings.tx_names.size()) ? wide(settings.tx_names[i])
                                                 : L"TX " + std::to_wstring(i + 1),
               12);
          meter(dc, {half() - 150, y + 10, 130, 8}, i < int(tx.size()) ? tx[i].get<float>() : 0.f);
        }
        if (i < settings.rx_channels) {
          text(dc, {half() + 12, y, half() - 200, 28},
               i < int(settings.rx_names.size()) ? wide(settings.rx_names[i])
                                                 : L"RX " + std::to_wstring(i + 1),
               12);
          meter(dc, {width - 168, y + 10, 130, 8}, i < int(rx.size()) ? rx[i].get<float>() : 0.f);
        }
        line(dc, 12, y + 28, width - 30, y + 28, border);
      }
    } else {
      int y = 317;
      for (const auto& s : live.value("endpoints", Json::array())) {
        if (y + 42 > bottom)
          break;
        const auto value = wide(s.value("name", "")) + L" · " +
                           std::to_wstring(s.value("nativeSampleRate", 0)) + L" Hz native · " +
                           std::to_wstring(s.value("nativeBufferFrames", 0)) + L" frames · queue " +
                           std::to_wstring(s.value("queuedFrames", 0)) + L" · drift " +
                           std::to_wstring(static_cast<int>(s.value("driftPpm", 0.))) +
                           L" ppm · underruns " + std::to_wstring(s.value("underruns", 0ULL));
        text(dc, {16, y, width - 42, 38}, value, 11, muted, FW_NORMAL, DT_WORDBREAK);
        y += 42;
      }
      if (!active)
        text(dc, {16, 317, width - 42, 36},
             L"Start audio to inspect native sample rates, buffers, resampling and drift "
             L"compensation.",
             12, muted);
    }
    line(dc, 12, bottom + 2, width - 12, bottom + 2, border);
    const auto band =
        (live.value("txBitsPerSecond", 0.) + live.value("rxBitsPerSecond", 0.)) / 1000.;
    const auto clock = live.value("clock", Json::object());
    const bool monitoring = clock.value("source", settings.clock_source) == "ptp-monitor";
    text(dc, {16, bottom + 10, width - 32, 25},
         L"Network: " +
             std::wstring(connected ? L"Connected"
                          : active  ? L"Waiting for controller"
                                    : L"Not connected") +
             (monitoring ? L"   ·   PTP monitor / audio free running   ·   "
                         : L"   ·   Clock: internal / free running   ·   ") +
             std::to_wstring(static_cast<int>(band)) + L" kbit/s",
         11, connected ? green : muted);
    text(dc, {16, bottom + 39, width - 32, 25},
         L"Packet loss: " + std::to_wstring(live.value("packetLoss", 0ULL)) + L"   ·   Late: " +
             std::to_wstring(live.value("latePackets", 0ULL)) + L"   ·   Underruns: " +
             std::to_wstring(live.value("bufferUnderruns", 0ULL)) + L"   ·   PTP: " +
             (monitoring ? wide(clock.value("state", "Stopped")) : L"Off"),
         11, muted);
    text(dc, {16, bottom + 68, width - 32, 24},
         monitoring ? L"PTP offset: " + clock_value(clock, "offsetNs") + L"   ·   Path delay: " +
                          clock_value(clock, "pathDelayNs") + L"   ·   Drift: " +
                          clock_value(clock, "frequencyPpm", 1., L" ppm") +
                          L"   ·   Double-click for details"
                    : L"WASAPI user mode · Windows performs device access without an OpenAoIP "
                      L"kernel driver.",
         10, muted);
  }
  void click(int x, int y, bool edit) override {
    if (edit && y >= height - 124 && y < height - 27) {
      MessageBoxW(window, clock_details(live.value("clock", Json::object())).c_str(),
                  L"OpenAoIP · Clock details", MB_OK | MB_ICONINFORMATION);
      return;
    }
    for (int i = 0; i < 3; ++i)
      if (Box{12 + i * 166, 281, 160, 29}.contains(x, y)) {
        tab = i;
        offset = 0;
        update_scroll();
        return;
      }
    if (y < 338 || y >= height - 124)
      return;
    const int row = (y - 338) / 29 + offset;
    if (tab == 0 && row < int(settings.endpoints.size())) {
      chosen = row;
      update_controls();
      if (edit && !active && !preview)
        edit_device(false);
    }
    if (tab == 1 && edit && !active && !preview) {
      const bool tx = x < half();
      const int n = tx ? settings.tx_channels : settings.rx_channels;
      if (row >= n)
        return;
      auto& names = tx ? settings.tx_names : settings.rx_names;
      while (int(names.size()) < n)
        names.push_back(std::string(tx ? "TX " : "RX ") + std::to_string(names.size() + 1));
      if (auto value = prompt(window, L"Network channel name", wide(names[row]))) {
        names[row] = utf8(*value);
        read();
        write_configuration(config_file, bridge::to_json(settings));
      }
    }
  }
  void scroll(int axis, int value) override {
    if (axis != SB_VERT)
      return;
    if (value <= -1000) {
      const int action = -1000 - value;
      if (action == SB_LINEUP)
        --offset;
      else if (action == SB_LINEDOWN)
        ++offset;
      else if (action == SB_PAGEUP)
        offset -= 8;
      else if (action == SB_PAGEDOWN)
        offset += 8;
    } else
      offset = value;
    offset = std::max(0, offset);
    update_scroll();
  }
};
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  Apartment apartment;
  enable_dpi();
  try {
    NetworkRuntime network;
    BridgeWindow app;
    std::filesystem::path preview;
    const auto args = arguments();
    for (std::size_t i = 0; i < args.size(); ++i) {
      if (args[i] == L"--data-dir" && i + 1 < args.size())
        app.folder = args[++i];
      else if (args[i] == L"--config" && i + 1 < args.size())
        app.config_file = args[++i];
      else if (args[i] == L"--engine-endpoint" && i + 1 < args.size())
        app.ipc = args[++i];
      else if (args[i] == L"--preview" && i + 1 < args.size())
        preview = args[++i];
      else
        throw std::runtime_error("Unknown Audio Bridge argument");
    }
    return app.run(instance, L"OpenAoIPAudioBridgeNative", L"OpenAoIP Audio Bridge", 1040, 740,
                   show, preview);
  } catch (const std::exception& e) {
    MessageBoxW(nullptr, wide(e.what()).c_str(), L"OpenAoIP Audio Bridge", MB_OK | MB_ICONERROR);
    return 1;
  }
}
