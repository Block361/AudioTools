#include "aoip/config_store.hpp"
#include "aoip/discovery.hpp"
#include "aoip/network.hpp"
#include "app_window.hpp"
#include "matrix_model.hpp"
#include <ctime>
#include <memory>
namespace {
using namespace aoip;
using namespace aoip::ui;
using namespace aoip::desktop;
enum Id { Save = 201, Load, Expand, Collapse, TxSearch, RxSearch, Online, HScroll, VScroll };
class ControllerWindow final : public AppWindow {
public:
  int port = 3210, tab = 0, sx = 0, sy = 0;
  std::filesystem::path folder;
  std::unique_ptr<Controller> controller;
  std::unique_ptr<DiscoveryServer> discovery;
  MatrixModel model;
  const std::vector<std::wstring> tabs = {L"Routing", L"Device Info", L"Clock Status",
                                          L"Network Status", L"Events"};
  static constexpr int gx = 280, gy = 312, cw = 30, rh = 24;
  Box tab_box(int n) const {
    return {12 + n * 132, 145, 128, 29};
  }
  Json visible_devices() const {
    auto result = Json::array();
    const auto needle = folded(model.tx_filter);
    for (const auto& d : model.state.at("devices")) {
      if (!needle.empty() &&
          folded(d.at("name").get<std::string>() + " " + d.at("id").get<std::string>())
                  .find(needle) == std::string::npos)
        continue;
      if (model.online_only && !d.value("online", false))
        continue;
      result.push_back(d);
    }
    return result;
  }
  void created() override {
    add_button(Save, L"Save preset");
    add_button(Load, L"Load preset");
    add_button(Expand, L"Expand all");
    add_button(Collapse, L"Collapse all");
    add(TxSearch, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL);
    add(RxSearch, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL);
    add_combo(Online, {L"All devices", L"Online only"});
    add(HScroll, L"SCROLLBAR", L"", SBS_HORZ);
    add(VScroll, L"SCROLLBAR", L"", SBS_VERT);
    if (preview) {
      model.update(preview_network());
      message = L"PREVIEW · Fixture data only; no live devices or routes.";
      message_color = amber;
      return;
    }
    if (folder.empty())
      folder = data_directory(L"Controller");
    ControllerConfig c;
    c.host = "0.0.0.0";
    c.port = port;
    c.state_file = (folder / L"routes.json").string();
    c.web_root = (executable_directory() / L"web").string();
    controller = std::make_unique<Controller>(c);
    try {
      discovery = std::make_unique<DiscoveryServer>(controller->port());
    } catch (const std::exception& e) {
      message = L"Discovery: " + wide(e.what());
      message_color = amber;
    }
    timer();
  }
  void layout() override {
    if (controls.empty())
      return;
    position(Save, {12, 65, 110, 28});
    position(Load, {130, 65, 110, 28});
    position(Expand, {252, 65, 105, 28});
    position(Collapse, {365, 65, 105, 28});
    const int edit = std::max(130, (width - 360) / 2);
    position(TxSearch, {91, 105, edit, 27});
    position(RxSearch, {188 + edit, 105, edit, 27});
    position(Online, {width - 155, 105, 143, 27});
    position(HScroll, {gx, height - 71, std::max(1, width - gx - 30), 14});
    position(VScroll, {width - 26, gy, 14, std::max(1, height - gy - 74)});
    update_scroll();
  }
  void update_scroll() {
    if (!window || !controls.contains(HScroll))
      return;
    const auto tx = model.transmitters(), rx = model.receivers();
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
    info.nMin = 0;
    info.nMax = tab == 0 ? std::max(0, int(rx.size()) - 1)
                         : std::max(0, int(tab == 4 ? model.state.at("events").size()
                                                    : visible_devices().size()) -
                                           1);
    info.nPage = std::max(1, tab == 0 ? (height - gy - 76) / rh : (height - 218) / 29);
    info.nPos = sy;
    sy = SetScrollInfo(controls[VScroll], SB_CTL, &info, TRUE);
    info.nMax = std::max(0, int(tx.size()) - 1);
    info.nPage = std::max(1, (width - gx - 30) / cw);
    info.nPos = sx;
    sx = SetScrollInfo(controls[HScroll], SB_CTL, &info, TRUE);
    ShowWindow(controls[HScroll], tab == 0 ? SW_SHOW : SW_HIDE);
    ShowWindow(controls[VScroll], SW_SHOW);
  }
  void timer() override {
    if (controller) {
      model.update(controller->state());
      message = std::to_wstring(model.state.at("devices").size()) + L" devices · " +
                std::to_wstring(model.state.at("routes").size()) + L" routes · TCP " +
                std::to_wstring(controller->port()) +
                L" · RTP audio flows directly between devices";
      if (!discovery) {
        message += L" · Discovery unavailable";
        message_color = amber;
      } else
        message_color = muted;
    }
    update_scroll();
  }
  void command(int id, int code) override {
    if (id == TxSearch || id == RxSearch || id == Online) {
      if (code == EN_CHANGE || code == CBN_SELCHANGE) {
        model.tx_filter = utf8(get_text(controls[TxSearch]));
        model.rx_filter = utf8(get_text(controls[RxSearch]));
        model.online_only = selection(Online) == 1;
        sx = sy = 0;
        update_scroll();
      }
      return;
    }
    if (id == Expand || id == Collapse) {
      model.expand_all(id == Expand);
      sx = sy = 0;
      update_scroll();
      return;
    }
    if (!controller)
      return;
    if (id == Save) {
      if (auto file = json_file_dialog(window, true, L"OpenAoIP-routing.json")) {
        write_configuration(*file, controller->preset());
        message = L"Routing preset saved.";
      }
    }
    if (id == Load) {
      if (auto file = json_file_dialog(window, false)) {
        controller->restore_preset(read_configuration(*file));
        timer();
        message = L"Preset restored atomically. Offline routes remain pending until their devices "
                  L"return.";
      }
    }
  }
  void paint(HDC dc) override {
    header(dc, L"OpenAoIP Controller", preview ? L"Preview" : L"Controller online",
           preview ? amber : green);
    text(dc, {12, 103, 76, 28}, L"TX / device", 11, muted);
    const int edit = std::max(130, (width - 360) / 2);
    text(dc, {103 + edit, 103, 82, 28}, L"RX search", 11, muted);
    text(dc, {490, 65, width - 505, 28},
         L"TX horizontal  /  RX vertical  ·  Click to route  ·  Double-click a label to rename", 11,
         muted);
    for (int i = 0; i < 5; ++i) {
      const auto b = tab_box(i);
      if (i == tab) {
        fill(dc, b, surface);
        line(dc, b.x, b.y + b.h - 1, b.x + b.w, b.y + b.h - 1, teal, 2);
      }
      text(dc, b, tabs[i], 12, i == tab ? ink : muted, i == tab ? FW_SEMIBOLD : FW_NORMAL,
           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    if (tab == 0)
      matrix(dc);
    else
      table(dc);
  }
  void matrix(HDC dc) {
    const auto tx = model.transmitters(), rx = model.receivers();
    fill(dc, {12, 179, width - 24, std::max(1, height - 253)}, surface);
    text(dc, {26, 195, gx - 40, 26}, L"RECEIVERS / RX", 12, ink, FW_SEMIBOLD);
    text(dc, {26, 226, gx - 42, 54},
         L"Expand a device with + / −.\nEach row and column is one audio channel.", 11, muted,
         FW_NORMAL, DT_WORDBREAK);
    if (tx.empty() || rx.empty()) {
      text(dc, {gx + 18, gy + 24, width - gx - 70, 100},
           L"No matching channels.\nStart a Virtual Soundcard or Audio Bridge,\nor adjust the "
           L"search and online filters.",
           13, muted, FW_NORMAL, DT_WORDBREAK);
      return;
    }
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, 12, 179, width - 30, height - 74);
    for (int col = sx; col < int(tx.size()); ++col) {
      const int x = gx + (col - sx) * cw;
      if (x >= width - 30)
        break;
      const auto& t = tx[col];
      fill(dc, {x, 180, cw, gy - 180}, t.group ? navy : surface);
      rotated(dc, {x, 189, cw, 91}, wide(t.group ? t.device_name : t.label), 11,
              t.online ? ink : gray);
      text(dc, {x, gy - 27, cw, 25},
           t.group ? (model.expanded_tx.contains(t.device) ? L"−" : L"+")
                   : std::to_wstring(t.number),
           11, t.group ? teal : muted, FW_SEMIBOLD, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    for (int row = sy; row < int(rx.size()); ++row) {
      const int y = gy + (row - sy) * rh;
      if (y + rh > height - 74)
        break;
      const auto& r = rx[row];
      fill(dc, {13, y, width - 43, rh}, r.group ? navy : row % 2 ? surface : background);
      line(dc, 13, y, width - 30, y, border);
      if (r.group) {
        text(dc, {24, y, 20, rh}, model.expanded_rx.contains(r.device) ? L"−" : L"+", 16, teal);
        dot(dc, 56, y + 12, r.online ? green : gray, 3);
        text(dc, {68, y, gx - 76, rh}, wide(r.device_name), 12, r.online ? ink : gray, FW_SEMIBOLD);
        continue;
      }
      text(dc, {30, y, 30, rh}, std::to_wstring(r.number), 10, muted);
      text(dc, {67, y, gx - 75, rh}, wide(r.label), 11, r.online ? ink : gray);
      const auto route = model.route_for(r);
      const auto state = model.connection_state(r);
      for (int col = sx; col < int(tx.size()); ++col) {
        const int x = gx + (col - sx) * cw;
        if (x + cw > width - 30)
          break;
        const auto& t = tx[col];
        if (t.group)
          continue;
        const bool connected = !route.is_null() && route.at("sender") == t.device &&
                               route.at("stream") == t.endpoint &&
                               route.at("senderChannel") == t.side;
        if (connected) {
          const auto color = !r.online || !t.online ? gray
                             : state == "receiving" ? green
                             : state == "waiting"   ? red
                                                    : amber;
          dot(dc, x + cw / 2, y + rh / 2, color, 7);
          text(dc, {x, y, cw, rh},
               state == "receiving" ? L"✓"
               : state == "waiting" ? L"!"
                                    : L"·",
               12, background, FW_SEMIBOLD, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
      }
    }
    for (int col = sx; col < int(tx.size()); ++col) {
      const int x = gx + (col - sx) * cw;
      if (x >= width - 30)
        break;
      line(dc, x, 179, x, height - 74, border);
    }
    line(dc, 12, gy, width - 30, gy, border);
    RestoreDC(dc, saved);
    int x = 16;
    for (const auto& item :
         std::vector<std::pair<COLORREF, std::wstring>>{{green, L"Receiving"},
                                                        {amber, L"Pending / buffering"},
                                                        {red, L"No packets / error"},
                                                        {gray, L"Offline / inactive"}}) {
      dot(dc, x, height - 38, item.first, 3);
      text(dc, {x + 10, height - 50, 166, 23}, item.second, 10, muted);
      x += 175;
    }
  }
  void table(HDC dc) {
    std::vector<std::wstring> heads =
        tab == 1   ? std::vector<std::wstring>{L"Device", L"State", L"IP address", L"TX / RX",
                                               L"Sample rate"}
        : tab == 2 ? std::vector<std::wstring>{L"Device", L"Clock source", L"Lock state",
                                               L"Sample rate", L"PTP"}
        : tab == 3 ? std::vector<std::wstring>{L"Device", L"IP address", L"TX RTP bytes",
                                               L"RX packets", L"Lost / late"}
                   : std::vector<std::wstring>{L"Time", L"Event"};
    const std::vector<int> sizes =
        tab == 4 ? std::vector<int>{115, width - 145}
                 : std::vector<int>{(width - 32) * 28 / 100, (width - 32) * 18 / 100,
                                    (width - 32) * 20 / 100, (width - 32) * 16 / 100,
                                    (width - 32) * 18 / 100};
    int x = 16;
    for (std::size_t i = 0; i < heads.size(); ++i) {
      text(dc, {x, 184, sizes[i] - 8, 28}, heads[i], 11, muted, FW_SEMIBOLD);
      x += sizes[i];
    }
    line(dc, 12, 214, width - 12, 214, border);
    int index = 0;
    if (tab == 4) {
      for (const auto& event : model.state.at("events")) {
        if (index++ < sy)
          continue;
        const int y = 216 + (index - sy - 1) * 29;
        if (y + 29 > height - 32)
          break;
        const auto stamp = event.value("at", 0LL) / 1000;
        std::time_t t = stamp;
        std::tm tm{};
        localtime_s(&tm, &t);
        wchar_t time[32]{};
        wcsftime(time, 32, L"%H:%M:%S", &tm);
        text(dc, {16, y, 115, 28}, time, 11, muted);
        text(dc, {131, y, width - 155, 28}, wide(event.value("message", "")), 12);
        line(dc, 12, y + 28, width - 12, y + 28, border);
      }
      return;
    }
    for (const auto& d : visible_devices()) {
      if (index++ < sy)
        continue;
      const int y = 216 + (index - sy - 1) * 29;
      if (y + 29 > height - 32)
        break;
      const int rate = d.value("format", Json::object()).value("sampleRate", 48000);
      int tx = 0, rx = 0;
      std::uint64_t bytes = 0, received = 0, lost = 0, late = 0;
      for (const auto& s : d.at("streams")) {
        tx += s.value("channels", 2);
        bytes += s.at("stats").value("bytes", 0ULL);
      }
      for (const auto& r : d.at("inputs")) {
        rx += r.value("channels", 2);
        const auto& s = r.at("stats");
        received += s.value("received", 0ULL);
        lost += s.value("lost", 0ULL);
        late += s.value("late", 0ULL);
      }
      const bool online = d.value("online", false);
      std::vector<std::wstring> values{wide(d.at("name"))};
      if (tab == 1)
        values.insert(values.end(), {online ? L"Online" : L"Offline", wide(d.at("host")),
                                     std::to_wstring(tx) + L" / " + std::to_wstring(rx),
                                     std::to_wstring(rate) + L" Hz"});
      if (tab == 2)
        values.insert(values.end(), {L"Internal / software", L"Free running",
                                     std::to_wstring(rate) + L" Hz", L"Not implemented"});
      if (tab == 3)
        values.insert(values.end(),
                      {wide(d.at("host")), std::to_wstring(bytes), std::to_wstring(received),
                       std::to_wstring(lost) + L" / " + std::to_wstring(late)});
      x = 16;
      for (std::size_t i = 0; i < values.size(); ++i) {
        text(dc, {x, y, sizes[i] - 8, 28}, values[i], 12,
             !online              ? gray
             : i == 1 && tab == 1 ? green
                                  : ink,
             i == 0 ? FW_SEMIBOLD : FW_NORMAL);
        x += sizes[i];
      }
      line(dc, 12, y + 28, width - 12, y + 28, border);
    }
  }
  void rename(const AxisItem& item, bool tx) {
    if (!controller)
      return;
    const auto title =
        item.group ? L"Device name (controller alias)" : L"Channel name (controller alias)";
    if (auto value = prompt(window, title, wide(item.group ? item.device_name : item.label))) {
      if (item.group)
        controller->set_device_name(item.device, utf8(*value));
      else
        controller->set_channel_name(item.device, tx ? "streams" : "inputs", item.endpoint,
                                     item.side, utf8(*value));
      timer();
    }
  }
  void click(int x, int y, bool edit) override {
    for (int i = 0; i < 5; ++i)
      if (tab_box(i).contains(x, y)) {
        tab = i;
        sx = sy = 0;
        update_scroll();
        return;
      }
    if (tab != 0) {
      if (edit && tab == 1 && y >= 216) {
        const int row = (y - 216) / 29 + sy;
        const auto ds = visible_devices();
        if (row >= 0 && row < int(ds.size())) {
          AxisItem a;
          a.device = ds[row].at("id");
          a.device_name = ds[row].at("name");
          a.group = true;
          rename(a, false);
        }
      }
      return;
    }
    if (x < 12 || x >= width - 30 || y < 179 || y >= height - 74)
      return;
    const auto tx = model.transmitters(), rx = model.receivers();
    const int col = (x - gx) / cw + sx, row = (y - gy) / rh + sy;
    if (x >= gx && y < gy && col >= 0 && col < int(tx.size())) {
      const auto& t = tx[col];
      if (edit) {
        rename(t, true);
        return;
      }
      if (t.group) {
        if (!model.expanded_tx.erase(t.device))
          model.expanded_tx.insert(t.device);
        update_scroll();
      }
      return;
    }
    if (y < gy || row < 0 || row >= int(rx.size()))
      return;
    const auto& r = rx[row];
    if (x < gx) {
      if (edit)
        rename(r, false);
      else if (r.group) {
        if (!model.expanded_rx.erase(r.device))
          model.expanded_rx.insert(r.device);
        update_scroll();
      }
      return;
    }
    if (edit || r.group || col < 0 || col >= int(tx.size()) || tx[col].group || !controller)
      return;
    const auto& t = tx[col];
    const auto current = model.route_for(r);
    if (!current.is_null() && current.at("sender") == t.device &&
        current.at("stream") == t.endpoint && current.at("senderChannel") == t.side)
      controller->remove_route(r.device, r.endpoint, r.side);
    else
      controller->set_route({{"sender", t.device},
                             {"stream", t.endpoint},
                             {"senderChannel", t.side},
                             {"receiver", r.device},
                             {"input", r.endpoint},
                             {"receiverChannel", r.side}});
    timer();
  }
  void scroll(int axis, int value) override {
    int& position = axis == SB_HORZ ? sx : sy;
    if (value <= -1000) {
      const int action = -1000 - value;
      if (action == SB_LINEUP)
        --position;
      else if (action == SB_LINEDOWN)
        ++position;
      else if (action == SB_PAGEUP)
        position -= 10;
      else if (action == SB_PAGEDOWN)
        position += 10;
    } else
      position = value;
    position = std::max(0, position);
    update_scroll();
  }
};
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  Apartment apartment;
  enable_dpi();
  try {
    NetworkRuntime network;
    ControllerWindow app;
    std::filesystem::path preview;
    const auto args = arguments();
    for (std::size_t i = 0; i < args.size(); ++i) {
      if (args[i] == L"--port" && i + 1 < args.size())
        app.port = std::stoi(args[++i]);
      else if (args[i] == L"--data-dir" && i + 1 < args.size())
        app.folder = args[++i];
      else if (args[i] == L"--preview" && i + 1 < args.size())
        preview = args[++i];
      else
        throw std::runtime_error("Unknown controller argument");
    }
    return app.run(instance, L"OpenAoIPControllerNative", L"OpenAoIP Controller", 1280, 800, show,
                   preview);
  } catch (const std::exception& e) {
    MessageBoxW(nullptr, wide(e.what()).c_str(), L"OpenAoIP Controller", MB_OK | MB_ICONERROR);
    return 1;
  }
}
