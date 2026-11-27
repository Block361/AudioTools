#pragma once
#include "aoip/bridge_engine.hpp"
#include "app_window.hpp"
#include <cmath>
#include <sstream>
namespace aoip::ui {
enum EditorId { EName = 301, EMode, EDevice, ELocal, ENetwork, EGain, ESave, ECancel };

inline std::wstring numbers(const std::vector<bridge::Patch>& patches, bool local) {
  std::wstring value;
  for (const auto& p : patches) {
    if (!value.empty())
      value += L", ";
    value += std::to_wstring(local ? p.local : p.network);
  }
  return value;
}
inline std::vector<int> channels(const std::wstring& value) {
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
  bool windows_audio = true;
  EndpointEditor(HWND parent, bridge::Endpoint endpoint, Json list)
      : value(std::move(endpoint)), inventory(std::move(list)) {
    windows_audio = inventory.value("loopbackSupported", true);
    owner = parent;
    modal = true;
  }
  void device_options(bool preserve) {
    const auto selected = preserve ? value.device : std::string("default");
    const bool capture = selection(EMode) == 0;
    SendMessageW(controls[EDevice], CB_RESETCONTENT, 0, 0);
    devices = {"default"};
    SendMessageW(controls[EDevice], CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(L"System default device"));
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
              windows_audio ? std::vector<std::wstring>{L"WASAPI capture → network TX",
                                                        L"WASAPI loopback → network TX",
                                                        L"Network RX → WASAPI playback"}
                            : std::vector<std::wstring>{L"ALSA capture → network TX",
                                                        L"Network RX → ALSA playback"},
              value.mode == EndpointMode::Capture ? 0
              : windows_audio                     ? (value.mode == EndpointMode::Loopback ? 1 : 2)
                                                  : 1);
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
    value.mode = selection(EMode) == 0                    ? EndpointMode::Capture
                 : windows_audio && selection(EMode) == 1 ? EndpointMode::Loopback
                                                          : EndpointMode::Playback;
    value.device = devices.at(selection(EDevice));
    value.patches.clear();
    for (std::size_t i = 0; i < local.size(); ++i)
      value.patches.push_back({local[i], net[i], gain});
    saved = true;
    DestroyWindow(window);
  }
  void paint(HDC dc) override {
    header(dc, windows_audio ? L"Audio Bridge · Local device" : L"OpenAoIP IoT · Audio device");
    for (const auto& row : std::vector<std::pair<int, std::wstring>>{{106, L"Label"},
                                                                     {146, L"Direction"},
                                                                     {186, L"Audio device"},
                                                                     {254, L"Local channels"},
                                                                     {294, L"Network channels"},
                                                                     {334, L"Gain (linear)"}})
      text(dc, {16, row.first, 156, 28}, row.second, 12, muted);
    text(dc, {180, 222, width - 196, 24},
         windows_audio && selection(EMode) == 1
             ? L"Loopback captures audio played through the selected Windows output."
             : L"Example stereo map: local 1, 2 → network 3, 4. Mono: 1 → 1.",
         11, muted);
    text(dc, {16, 379, width - 32, 48},
         L"Capture/loopback feed TX. Playback consumes RX. Use one entry per device and direction; "
         L"add all its channel assignments here.",
         11, muted, FW_NORMAL, DT_WORDBREAK);
  }
};

} // namespace aoip::ui
