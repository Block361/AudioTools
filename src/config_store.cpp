#include "aoip/config_store.hpp"
#include <fstream>
#ifdef _WIN32
#include <shlobj.h>
#include <windows.h>
#endif
namespace aoip {
Json read_configuration(const std::filesystem::path& file, std::size_t max_bytes) {
  if (std::filesystem::file_size(file) > max_bytes)
    throw std::runtime_error("Configuration file is too large");
  std::ifstream input(file);
  if (!input)
    throw std::runtime_error("Cannot read configuration file");
  Json result;
  input >> result;
  return result;
}
void write_configuration(const std::filesystem::path& file, const Json& value) {
  std::filesystem::create_directories(std::filesystem::absolute(file).parent_path());
  const auto temp = file.wstring() + L".tmp";
  {
    std::ofstream output(std::filesystem::path(temp), std::ios::binary | std::ios::trunc);
    output << value.dump(2);
    output.close();
    if (!output)
      throw std::runtime_error("Cannot write configuration");
  }
#ifdef _WIN32
  if (!MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    throw std::runtime_error("Cannot replace configuration file");
#else
  std::filesystem::rename(temp, file);
#endif
}
std::filesystem::path engine_endpoint_file() {
#ifdef _WIN32
  PWSTR value = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &value)))
    throw std::runtime_error("Cannot locate the per-user engine folder");
  auto folder = std::filesystem::path(value) / L"OpenAoIP" / L"Engine";
  CoTaskMemFree(value);
  DWORD session = 0;
  ProcessIdToSessionId(GetCurrentProcessId(), &session);
  return folder / (L"session-" + std::to_wstring(session) + L".json");
#else
  throw std::runtime_error("Native engine IPC currently requires Windows");
#endif
}
} // namespace aoip
