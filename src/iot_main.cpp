// SPDX-License-Identifier: MIT
#include "aoip/config_store.hpp"
#include "aoip/iot.hpp"
#include "aoip/network.hpp"
#include "aoip/version.hpp"
#include <csignal>
#include <iostream>
#include <thread>
namespace {
volatile std::sig_atomic_t stopping = 0;
void stop(int) {
  stopping = 1;
}
} // namespace
int main(int argc, char** argv) {
  try {
    aoip::NetworkRuntime network;
    std::filesystem::path file = "iot.json";
    std::string action = "run";
    for (int i = 1; i < argc; ++i) {
      const std::string arg = argv[i];
      if (arg == "--config" && i + 1 < argc)
        file = argv[++i];
      else if (arg == "--init")
        action = "init";
      else if (arg == "--check-config")
        action = "check";
      else if (arg == "--audio-devices")
        action = "devices";
      else if (arg == "--version") {
        std::cout << "OpenAoIP IoT " << aoip::iot::version << '\n';
        return 0;
      } else if (arg == "--help") {
        std::cout
            << "OpenAoIP IoT - headless ALSA network audio\n"
               "  openaoip-iot --init --config iot.json\n"
               "  openaoip-iot --audio-devices\n"
               "  openaoip-iot --check-config --config iot.json\n"
               "  openaoip-iot --config iot.json\n"
               "Copy the private management key from iot.json into Controller > Manage IoT.\n";
        return 0;
      } else
        throw std::runtime_error("Unknown argument: " + arg);
    }
    if (action == "init") {
      if (std::filesystem::exists(file))
        throw std::runtime_error("Configuration exists; initialization will not overwrite it");
      aoip::write_configuration(file, aoip::iot::to_json(aoip::iot::initial_configuration(), true));
      std::cout << "Created " << file
                << ". Set the device/controller addresses and audio endpoints; keep its management "
                   "key private.\n";
      return 0;
    }
    if (action == "devices") {
      std::cout << aoip::iot::audio_inventory().dump(2) << '\n';
      return 0;
    }
    if (action == "check") {
      aoip::iot::configuration_from_json(aoip::read_configuration(file, 128 * 1024));
      std::cout << "OpenAoIP IoT configuration is valid; hardware was not opened.\n";
      return 0;
    }
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    aoip::iot::Host host(file);
    std::cout << "OpenAoIP IoT " << aoip::iot::version << " management port " << host.port()
              << "; Ctrl+C stops the device.\n";
    while (!stopping)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "OpenAoIP IoT: " << e.what() << '\n';
    return 1;
  }
}
