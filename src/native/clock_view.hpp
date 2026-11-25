#pragma once
#include "aoip/system.hpp"
#include "ui.hpp"
#include <iomanip>
#include <sstream>
namespace aoip::ui {
inline int ptp_selection(const std::string& source) {
  return source == "ptp-grandmaster" ? 2 : source == "ptp-monitor" ? 1 : 0;
}
inline std::string ptp_source(int selected) {
  if (selected == 0)
    return "internal";
  if (selected == 1)
    return "ptp-monitor";
  if (selected == 2)
    return "ptp-grandmaster";
  throw std::runtime_error("Select a clock role");
}
inline std::wstring clock_value(const Json& clock, const char* field, double divisor = 1000.,
                                const wchar_t* unit = L" µs") {
  if (!clock.contains(field) || !clock.at(field).is_number())
    return L"—";
  std::wostringstream out;
  out << std::fixed << std::setprecision(1) << clock.at(field).get<double>() / divisor << unit;
  return out.str();
}
inline int ptp_domain(HWND edit) {
  const auto raw = get_text(edit);
  if (raw.empty() || raw.size() > 3 ||
      std::any_of(raw.begin(), raw.end(), [](wchar_t c) { return c < L'0' || c > L'9'; }))
    throw std::runtime_error("PTP domain must be 0-255");
  std::size_t end = 0;
  const int value = std::stoi(raw, &end);
  if (end != raw.size() || value < 0 || value > 255)
    throw std::runtime_error("PTP domain must be 0-255");
  return value;
}
inline std::wstring audio_clock_state(const Json& clock) {
  if (clock.value("audioLocked", false))
    return L"Sample locked";
  if (clock.value("audioRateLocked", false))
    return clock.value("source", "") == "ptp-grandmaster" ? L"Software reference"
                                                          : L"PTP rate locked";
  if (clock.value("audioRateActive", false))
    return clock.value("rateState", "") == "Holdover" ? L"Holdover" : L"Acquiring rate";
  return L"Free running";
}
inline std::wstring clock_details(const Json& clock) {
  return L"Mode: " + wide(clock.value("mode", "Internal / software")) + L"\nState: " +
         wide(clock.value("state", "Free running")) + L"\nAudio clock: " +
         audio_clock_state(clock) +
         L"\nPhase/sample synchronization: no\nApplied network rate correction: " +
         clock_value(clock, "rateCorrectionPpm", 1., L" ppm") + L"\nRate servo: " +
         wide(clock.value("rateState", "Off")) + L"\n\nGrandmaster: " +
         (clock.value("grandmasterIdentity", Json()).is_string()
              ? wide(clock.at("grandmasterIdentity"))
              : L"—") +
         L"\nMaster address: " +
         (clock.value("masterAddress", Json()).is_string() ? wide(clock.at("masterAddress"))
                                                           : L"—") +
         L"\nDomain: " + std::to_wstring(clock.value("domain", 0)) +
         L"\nOffset (master − local session clock): " + clock_value(clock, "offsetNs") +
         L"\nMean path delay: " + clock_value(clock, "pathDelayNs") + L"\nRelative frequency: " +
         clock_value(clock, "frequencyPpm", 1., L" ppm") + L"\nTimestamping: " +
         wide(clock.value("timestamping", "not active")) + L"\nFresh measurement: " +
         (clock.value("measurementFresh", false) ? L"yes" : L"no") + L"\n\n" +
         wide(clock.value("error", std::string{})) + L"\n" +
         wide(clock.value("warning", std::string{})) + L"\nReference: " +
         wide(clock.value("clockReference", "Local session clock"));
}
} // namespace aoip::ui
