#pragma once
#include "aoip/system.hpp"
#include "ui.hpp"
#include <iomanip>
#include <sstream>
namespace aoip::ui {
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
  std::size_t end = 0;
  const int value = std::stoi(raw, &end);
  if (end != raw.size() || value < 0 || value > 255)
    throw std::runtime_error("PTP domain must be 0-255");
  return value;
}
inline std::wstring clock_details(const Json& clock) {
  return L"Mode: " + wide(clock.value("mode", "Internal / software")) + L"\nState: " +
         wide(clock.value("state", "Free running")) +
         L"\nAudio clock: free running (PTP discipline pending)\n\nGrandmaster: " +
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
         wide(clock.value("error", std::string{}));
}
} // namespace aoip::ui
