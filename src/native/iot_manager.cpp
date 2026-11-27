// SPDX-License-Identifier: MIT
#include "iot_manager.hpp"
#include "aoip/config_store.hpp"
#include "aoip/iot.hpp"
#include "aoip/network.hpp"
#include "clock_view.hpp"
#include "endpoint_editor.hpp"
#include <future>
namespace aoip::ui {
namespace {
enum Id {
  Device = 701,
  Key,
  Connect,
  Apply,
  Start,
  Stop,
  Endpoints,
  Add,
  Edit,
  Remove,
  Name,
  Interface,
  Tx,
  Rx,
  Rate,
  Buffer,
  NetworkLatency,
  LocalLatency,
  Clock,
  Domain,
  TxNames,
  RxNames,
  Export,
  Import,
  Diagnostics
};
class DiagnosticWindow final : public AppWindow {
public:
  Json value;
  DiagnosticWindow(HWND parent, Json state) : value(std::move(state)) {
    owner = parent;
    modal = true;
  }
  void created() override {
    std::wstring body;
    for (const auto ch : wide(value.dump(2))) {
      if (ch == L'\n')
        body += L'\r';
      body += ch;
    }
    add(1, L"EDIT", body,
        WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOHSCROLL | ES_AUTOVSCROLL | WS_VSCROLL |
            WS_HSCROLL);
  }
  void layout() override {
    if (!controls.empty())
      position(1, {12, 65, width - 24, height - 104});
  }
  void paint(HDC dc) override {
    header(dc, L"OpenAoIP IoT · Live diagnostics");
  }
};
std::wstring join_names(const std::vector<std::string>& names) {
  std::wstring out;
  for (const auto& n : names) {
    if (!out.empty())
      out += L", ";
    out += wide(n);
  }
  return out;
}
std::vector<std::string> split_names(const std::wstring& text) {
  std::vector<std::string> out;
  std::wistringstream stream(text);
  std::wstring part;
  while (std::getline(stream, part, L',')) {
    const auto first = part.find_first_not_of(L" \t"), last = part.find_last_not_of(L" \t");
    out.push_back(first == std::wstring::npos ? "" : utf8(part.substr(first, last - first + 1)));
  }
  return out;
}
class Manager final : public AppWindow {
public:
  Json devices, inventory = Json::object(), live = Json::object();
  bridge::Settings settings;
  std::uint64_t revision = 0;
  std::string preferred;
  std::optional<iot::Client> client;
  std::vector<std::string> interfaces;
  std::future<Json> pending;
  bool connected = false, background_poll = false;
  double next_poll = 0;
  Manager(HWND parent, Json list, std::string chosen)
      : devices(std::move(list)), preferred(std::move(chosen)) {
    owner = parent;
    modal = true;
  }
  bool busy() const {
    return pending.valid();
  }
  void refresh_controls() {
    for (int id :
         {Apply, Start,     Stop,    Add,     Edit,      Remove,     Export,         Import,
          Name,  Interface, Tx,      Rx,      Rate,      Buffer,     NetworkLatency, LocalLatency,
          Clock, Domain,    TxNames, RxNames, Endpoints, Diagnostics})
      EnableWindow(controls.at(id), connected && !busy());
    for (int id : {Device, Key, Connect})
      EnableWindow(controls.at(id), !busy());
  }
  void created() override {
    std::vector<std::wstring> labels;
    int selected = 0;
    for (const auto& d : devices) {
      if (d.at("id") == preferred)
        selected = int(labels.size());
      labels.push_back(wide(d.at("name")) + L" · " + wide(d.at("host")));
    }
    add_combo(Device, labels, selected);
    add(Key, L"EDIT", L"", WS_BORDER | ES_PASSWORD | ES_AUTOHSCROLL);
    SendMessageW(controls[Key], EM_SETLIMITTEXT, 64, 0);
    add_button(Connect, L"Connect / refresh");
    add_button(Apply, L"Apply & restart");
    add_button(Start, L"Start");
    add_button(Stop, L"Stop");
    add_button(Export, L"Export audio");
    add_button(Import, L"Import audio");
    add_button(Diagnostics, L"Diagnostics");
    add(Name, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL);
    add_combo(Interface, {});
    for (int id : {Tx, Rx, Buffer, NetworkLatency, LocalLatency, Domain})
      add(id, L"EDIT", L"", WS_BORDER | ES_NUMBER);
    add_combo(Rate, {L"44100 Hz", L"48000 Hz", L"96000 Hz"}, 1);
    add_combo(Clock, {L"Internal", L"PTP follower", L"Software GM"});
    add(TxNames, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL);
    add(RxNames, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL);
    add(Endpoints, L"LISTBOX", L"", WS_BORDER | WS_VSCROLL | LBS_NOTIFY);
    add_button(Add, L"Add device");
    add_button(Edit, L"Edit map");
    add_button(Remove, L"Remove");
    message =
        devices.empty()
            ? L"No OpenAoIP IoT devices discovered. Start the IoT service on the Pi first."
            : L"Enter the private management key from the device configuration, then connect.";
    refresh_controls();
  }
  void layout() override {
    if (controls.empty())
      return;
    const int half = (width - 42) / 2;
    position(Device, {16, 82, half, 28});
    position(Key, {half + 28, 82, half - 168, 28});
    position(Connect, {width - 176, 82, 160, 28});
    int x = 16;
    for (int id : {Apply, Start, Stop, Export, Import, Diagnostics}) {
      const int w = id == Apply ? 145 : id == Start || id == Stop ? 70 : 115;
      position(id, {x, 124, w, 28});
      x += w + 8;
    }
    position(Name, {130, 174, half - 114, 28});
    position(Interface, {half + 140, 174, half - 112, 28});
    position(Tx, {130, 214, 80, 28});
    position(Rx, {278, 214, 80, 28});
    position(Rate, {half + 140, 214, half - 112, 28});
    position(Buffer, {130, 254, 80, 28});
    position(NetworkLatency, {278, 254, 80, 28});
    position(LocalLatency, {half + 140, 254, half - 112, 28});
    position(Clock, {130, 294, half - 114, 28});
    position(Domain, {half + 140, 294, 90, 28});
    position(TxNames, {130, 334, width - 146, 28});
    position(RxNames, {130, 374, width - 146, 28});
    position(Endpoints, {16, 444, width - 210, std::max(50, height - 570)});
    position(Add, {width - 178, 444, 162, 28});
    position(Edit, {width - 178, 480, 162, 28});
    position(Remove, {width - 178, 516, 162, 28});
  }
  void load_fields() {
    set_text(controls[Name], wide(settings.name));
    interfaces = {"0.0.0.0"};
    for (const auto& ip : inventory.value("interfaces", Json::array()))
      interfaces.push_back(ip.get<std::string>());
    if (std::find(interfaces.begin(), interfaces.end(), settings.interface_ip) == interfaces.end())
      interfaces.push_back(settings.interface_ip);
    SendMessageW(controls[Interface], CB_RESETCONTENT, 0, 0);
    int interface_index = 0;
    for (std::size_t i = 0; i < interfaces.size(); ++i) {
      const auto label = wide(interfaces[i]);
      SendMessageW(controls[Interface], CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
      if (interfaces[i] == settings.interface_ip)
        interface_index = int(i);
    }
    select(Interface, interface_index);
    for (auto [id, value] : {std::pair{Tx, settings.tx_channels},
                             {Rx, settings.rx_channels},
                             {Buffer, settings.buffer_frames},
                             {NetworkLatency, settings.latency_ms},
                             {LocalLatency, settings.local_latency_ms},
                             {Domain, settings.ptp_domain}})
      set_text(controls[id], std::to_wstring(value));
    select(Rate, settings.rate == 44100 ? 0 : settings.rate == 96000 ? 2 : 1);
    select(Clock, ptp_selection(settings.clock_source));
    set_text(controls[TxNames], join_names(settings.tx_names));
    set_text(controls[RxNames], join_names(settings.rx_names));
    list_endpoints();
  }
  int integer(int id) {
    const auto text = get_text(controls.at(id));
    std::size_t end = 0;
    const int value = std::stoi(text, &end);
    if (end != text.size())
      throw std::runtime_error("Enter an integer in each numeric field");
    return value;
  }
  bridge::Settings read_fields() {
    auto s = settings;
    s.name = utf8(get_text(controls[Name]));
    s.interface_ip = interfaces.at(selection(Interface));
    s.tx_channels = integer(Tx);
    s.rx_channels = integer(Rx);
    s.buffer_frames = integer(Buffer);
    s.latency_ms = integer(NetworkLatency);
    s.local_latency_ms = integer(LocalLatency);
    s.ptp_domain = integer(Domain);
    s.rate = selection(Rate) == 0 ? 44100 : selection(Rate) == 2 ? 96000 : 48000;
    s.clock_source = ptp_source(selection(Clock));
    s.tx_names = split_names(get_text(controls[TxNames]));
    s.rx_names = split_names(get_text(controls[RxNames]));
    bridge::validate(s, live.value("running", false));
    return s;
  }
  void list_endpoints() {
    SendMessageW(controls[Endpoints], LB_RESETCONTENT, 0, 0);
    for (const auto& e : settings.endpoints) {
      const auto label =
          wide(e.name) +
          (e.mode == EndpointMode::Playback ? L"   RX → playback   " : L"   capture → TX   ") +
          numbers(e.patches, true) + L" → " + numbers(e.patches, false);
      SendMessageW(controls[Endpoints], LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
  }
  void queue(std::function<Json()> work, bool background = false) {
    if (busy())
      return;
    pending = std::async(std::launch::async, std::move(work));
    background_poll = background;
    if (!background) {
      message = L"Contacting IoT device…";
      refresh_controls();
    }
  }
  static Json snapshot(const iot::Client& c) {
    return {{"config", c.configuration()}, {"inventory", c.inventory()}, {"status", c.status()}};
  }
  void timer() override {
    if (busy() && pending.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
      try {
        const auto value = pending.get();
        live = value.at("status");
        if (value.contains("config")) {
          settings = bridge::from_json(value.at("config").at("audio"));
          revision = value.at("config").at("revision");
          inventory = value.at("inventory");
          connected = true;
          load_fields();
          message = L"Connected · Configuration loaded from device";
        } else if (live.value("revision", revision) != revision)
          message = L"Device configuration changed elsewhere. Refresh before applying.";
        else
          message = L"Connected · Live status";
        message_color = live.value("running", false) ? green : gray;
      } catch (const std::exception& e) {
        message = wide(e.what());
        message_color = red;
      }
      background_poll = false;
      next_poll = monotonic_ms() + 3000;
      refresh_controls();
    }
    if (connected && !busy() && monotonic_ms() >= next_poll) {
      auto c = *client;
      queue([c] { return Json{{"status", c.status()}}; }, true);
    }
  }
  void command(int id, int notice) override {
    if (id == Device && notice == CBN_SELCHANGE) {
      connected = false;
      client.reset();
      set_text(controls[Key], L"");
      refresh_controls();
      return;
    }
    if (busy()) {
      message = L"Device request in progress; try again shortly.";
      return;
    }
    if (id == Connect) {
      if (devices.empty())
        throw std::runtime_error("No IoT devices discovered");
      const auto& d = devices.at(selection(Device));
      connected = false;
      client.emplace(d.at("host"), d.at("management").at("port"), utf8(get_text(controls[Key])),
                     d.at("id"));
      auto c = *client;
      queue([c] { return snapshot(c); });
      return;
    }
    if (!connected)
      return;
    if (id == Diagnostics) {
      DiagnosticWindow viewer(window, live);
      viewer.run(GetModuleHandleW(nullptr), L"OpenAoIPIoTDiagnostics",
                 L"OpenAoIP IoT · Diagnostics", 900, 700, SW_SHOW);
      return;
    }
    if (id == Apply) {
      const auto s = read_fields();
      auto c = *client;
      const auto rev = revision;
      queue([c, rev, s] {
        c.configure(rev, bridge::to_json(s));
        return snapshot(c);
      });
      return;
    }
    if (id == Start || id == Stop) {
      auto c = *client;
      const auto rev = revision;
      queue([c, rev, id] {
        c.set_running(rev, id == Start);
        return snapshot(c);
      });
      return;
    }
    if (id == Export) {
      if (auto file = json_file_dialog(window, true, L"OpenAoIP-IoT-audio.json"))
        write_configuration(*file, bridge::to_json(read_fields()));
      return;
    }
    if (id == Import) {
      if (auto file = json_file_dialog(window, false)) {
        auto s = bridge::from_json(read_configuration(*file, 128 * 1024));
        s.id = settings.id;
        s.controller = settings.controller;
        bridge::validate(s, false);
        settings = std::move(s);
        load_fields();
        message = L"Imported locally; Apply sends and saves these settings on the device.";
      }
      return;
    }
    const int selected = int(SendMessageW(controls[Endpoints], LB_GETCURSEL, 0, 0));
    if (id == Remove && selected >= 0 && selected < int(settings.endpoints.size())) {
      settings.endpoints.erase(settings.endpoints.begin() + selected);
      list_endpoints();
    }
    if (id == Add || id == Edit || (id == Endpoints && notice == LBN_DBLCLK)) {
      const bool add = id == Add;
      if (!add && (selected < 0 || selected >= int(settings.endpoints.size())))
        return;
      bridge::Endpoint e = add ? bridge::Endpoint{"endpoint-" + random_id(),
                                                  "Audio device",
                                                  "default",
                                                  EndpointMode::Capture,
                                                  {{1, 1, 1}, {2, 2, 1}}}
                               : settings.endpoints[selected];
      EndpointEditor editor(window, std::move(e), inventory);
      editor.run(GetModuleHandleW(nullptr), L"OpenAoIPIoTEndpointEditor",
                 L"OpenAoIP IoT · Audio device", 720, 500, SW_SHOW);
      if (editor.saved) {
        if (add)
          settings.endpoints.push_back(editor.value);
        else
          settings.endpoints[selected] = editor.value;
        list_endpoints();
      }
    }
  }
  void paint(HDC dc) override {
    header(dc, L"OpenAoIP IoT · Device management", connected ? L"Connected" : L"Not paired",
           connected ? green : gray);
    const int half = (width - 42) / 2;
    text(dc, {16, 62, half, 18}, L"Discovered IoT device", 11, muted);
    text(dc, {half + 28, 62, half - 180, 18}, L"Management key (kept in memory)", 11, muted);
    for (const auto& [y, label] : std::vector<std::pair<int, std::wstring>>{{174, L"Device name"},
                                                                            {214, L"TX channels"},
                                                                            {254, L"I/O frames"},
                                                                            {294, L"Clock source"},
                                                                            {334, L"TX names"},
                                                                            {374, L"RX names"}})
      text(dc, {16, y, 110, 28}, label, 11, muted);
    text(dc, {224, 214, 50, 28}, L"RX", 11, muted);
    text(dc, {224, 254, 50, 28}, L"Net ms", 11, muted);
    for (const auto& [y, label] : std::vector<std::pair<int, std::wstring>>{
             {174, L"LAN IPv4"}, {214, L"Sample rate"}, {254, L"Local ms"}, {294, L"PTP domain"}})
      text(dc, {half + 28, y, 106, 28}, label, 11, muted);
    text(dc, {16, 414, width - 32, 22},
         L"AUDIO DEVICES / CHANNEL MAPS · names above are comma-separated", 11, muted);
    const int y = height - 112;
    text(dc, {16, y, width - 32, 22},
         L"Audio: " +
             wide(live.value("state", live.value("running", false) ? "Running" : "Stopped")) +
             L"   ·   " + audio_clock_state(live.value("clock", Json::object())) +
             L"   ·   Lost: " + std::to_wstring(live.value("packetLoss", 0ULL)) +
             L"   ·   Underruns: " + std::to_wstring(live.value("bufferUnderruns", 0ULL)),
         11);
    const auto peaks = live.value("rxPeaks", Json::array());
    text(dc, {16, y + 28, 80, 20}, L"RX peak", 10, muted);
    float peak = 0;
    for (const auto& p : peaks)
      peak = std::max(peak, p.get<float>());
    meter(dc, {100, y + 33, 150, 10}, peak);
    const auto error = live.value("startError", std::string{});
    text(dc, {272, y + 28, width - 288, 35},
         wide(error.empty() ? live.value("managementError", std::string{}) : error), 10, amber,
         FW_NORMAL, DT_WORDBREAK);
  }
};
} // namespace
void manage_iot(HWND owner, const Json& devices, const std::string& selected) {
  auto list = Json::array();
  for (const auto& d : devices)
    if (d.value("online", false) && d.contains("management") &&
        d.at("management").value("product", "") == "OpenAoIP IoT")
      list.push_back(d);
  Manager manager(owner, std::move(list), selected);
  manager.run(GetModuleHandleW(nullptr), L"OpenAoIPIoTManager", L"OpenAoIP IoT · Management", 1020,
              760, SW_SHOW);
}
} // namespace aoip::ui
