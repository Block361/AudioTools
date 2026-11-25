#include "aoip/engine_host.hpp"
#include "aoip/network.hpp"
#include <iostream>
#include <windows.h>
namespace {
aoip::engine::Host* active = nullptr;
BOOL WINAPI stop(DWORD) {
  if (active)
    active->stop();
  return TRUE;
}
} // namespace
int wmain(int argc, wchar_t** argv) {
  try {
    aoip::NetworkRuntime network;
    aoip::engine::Options options;
    std::filesystem::path config;
    for (int i = 1; i < argc; ++i) {
      const std::wstring arg = argv[i];
      if (arg == L"--endpoint-file" && i + 1 < argc)
        options.endpoint_file = argv[++i];
      else if (arg == L"--idle-seconds" && i + 1 < argc)
        options.idle_seconds = std::stoi(argv[++i]);
      else if (arg == L"--bridge-config" && i + 1 < argc)
        config = argv[++i];
      else
        throw std::runtime_error("Unknown engine argument");
    }
    aoip::engine::Host host(options);
    active = &host;
    SetConsoleCtrlHandler(stop, TRUE);
    if (!config.empty())
      host.start("bridge", aoip::read_configuration(config));
    host.run();
    active = nullptr;
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
