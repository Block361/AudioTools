#include "aoip/network.hpp"
#include "aoip/system.hpp"
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <thread>

namespace {
volatile std::sig_atomic_t stopping = 0;
void stop(int) {
  stopping = 1;
}
using namespace std::chrono_literals;
struct Options {
  std::map<std::string, std::vector<std::string>> values;
  std::string get(const std::string& key, const std::string& fallback) const {
    const auto it = values.find(key);
    return it == values.end() ? fallback : it->second.back();
  }
  int number(const std::string& key, int fallback, int low, int high) const {
    const auto text = get(key, std::to_string(fallback));
    std::size_t end = 0;
    const auto value = std::stoll(text, &end);
    if (end != text.size() || value < low || value > high)
      throw std::runtime_error(key + " must be " + std::to_string(low) + "-" +
                               std::to_string(high));
    return static_cast<int>(value);
  }
};
std::vector<int> mapping(const Options& options, const std::string& key) {
  const auto text = options.get(key, "");
  if (text.empty())
    return {};
  std::vector<int> result;
  std::istringstream stream(text);
  std::string part;
  while (std::getline(stream, part, ',')) {
    std::size_t end = 0;
    const auto value = std::stoll(part, &end);
    if (end != part.size() || value < 1 || value > 32)
      throw std::runtime_error(key + " expects comma-separated channel numbers 1-32");
    result.push_back(static_cast<int>(value));
  }
  if (text.back() == ',')
    throw std::runtime_error("Trailing comma in " + key);
  return result;
}
Options device_options(Options options) {
  if (!options.values.contains("--config"))
    return options;
  const auto path = options.get("--config", "");
  if (std::filesystem::file_size(path) > 65536)
    throw std::runtime_error("Device configuration exceeds 64 KiB");
  std::ifstream file(path);
  aoip::Json config;
  file >> config;
  if (!config.is_object())
    throw std::runtime_error("Device configuration must be a JSON object");
  const std::map<std::string, std::string> keys = {{"id", "--id"},
                                                   {"name", "--name"},
                                                   {"controller", "--controller"},
                                                   {"host", "--host"},
                                                   {"streams", "--streams"},
                                                   {"inputs", "--inputs"},
                                                   {"capture", "--capture"},
                                                   {"playback", "--playback"},
                                                   {"captureMap", "--capture-map"},
                                                   {"playbackMap", "--playback-map"},
                                                   {"volume", "--volume"},
                                                   {"latency", "--latency"},
                                                   {"port", "--port"},
                                                   {"monitorPort", "--monitor-port"},
                                                   {"tone", "--tone"},
                                                   {"wav", "--wav"}};
  const std::set<std::string> text_keys = {"id",   "name",    "controller",
                                           "host", "capture", "playback"};
  for (const auto& [key, value] : config.items()) {
    const auto it = keys.find(key);
    if (it == keys.end())
      throw std::runtime_error("Unknown device config field: " + key);
    if (options.values.contains(it->second))
      continue; // CLI overrides the file.
    if (key == "captureMap" || key == "playbackMap") {
      if (!value.is_array())
        throw std::runtime_error(key + " must be an array of channel numbers");
      std::string text;
      for (const auto& channel : value) {
        if (!channel.is_number_integer())
          throw std::runtime_error("Invalid channel map");
        if (!text.empty())
          text += ',';
        text += channel.dump();
      }
      options.values[it->second] = {text};
    } else if (key == "wav") {
      if (!value.is_array())
        throw std::runtime_error("wav must be an array of file paths");
      for (const auto& item : value) {
        // WAV paths in a config file are relative to that file, not the launch directory.
        auto audio_path = std::filesystem::path(item.get<std::string>());
        if (audio_path.is_relative())
          audio_path = std::filesystem::absolute(path).parent_path() / audio_path;
        options.values[it->second].push_back(audio_path.string());
      }
    } else if (text_keys.contains(key)) {
      if (!value.is_string())
        throw std::runtime_error(key + " must be a string");
      options.values[it->second] = {value.get<std::string>()};
    } else {
      if (!value.is_number_integer())
        throw std::runtime_error(key + " must be an integer");
      options.values[it->second] = {value.dump()};
    }
  }
  return options;
}
void help() {
  std::cout << R"(OpenAoIP 0.6.3 | C++20 audio-over-IP prototype

  openaoip demo [--port 3210]
  openaoip controller [--host 127.0.0.1] [--port 3210]
  openaoip sender --id stage [--streams 2] [--tone 440]
  openaoip receiver --id speakers [--inputs 2] [--port 5004]
  openaoip audio-devices [--json]
  openaoip hardware [--capture default] [--playback default]
  openaoip device --id stage --controller http://192.168.1.10:3210
  openaoip device --config examples/device-stage.json

Shared endpoint options:
  --name TEXT          Human-readable device name
  --controller URL     Controller address (default http://127.0.0.1:3210)
  --host IPV4          Local bind address (devices default 0.0.0.0)
Network device mode:
  One device ID registers both its sending and receiving streams.
  --config FILE        JSON configuration; explicit CLI options override it
  --id ID             Unique, stable device ID (required unless in config)
  --streams 0..16     Sending stereo pairs (default 1; 0 disables sending)
  --inputs 0..16      Receiving stereo pairs (default 1; 0 disables receiving)
  --capture DEVICE    Microphone/sound card (default: system default)
  --tone HZ / --wav   Use test tones or WAV instead of physical capture
  --playback off      Receive and monitor without opening physical speakers
  --port PORT         First receive UDP port (default 5004; 0 = automatic)
  --monitor-port PORT Capture monitoring TCP port (default 5100; 0 = automatic)
  Channel maps, volume, latency and hardware selection work as below.
Sender options:
  --streams 1..16      Independent stereo streams (default 2)
  --tone 20..18000     First test tone frequency; each stream adds 110 Hz
  --wav FILE          Loop a 48 kHz, 16-bit stereo WAV; repeat for more streams
  --capture DEVICE    Microphone/sound card: default, index or exact device name
  --capture-map LIST  Physical input channels, e.g. 1,1 for mono or 1,2,3,4
Receiver options:
  --inputs 1..16       Independent stereo inputs (default 2)
  --port PORT         First UDP port; inputs use consecutive ports; 0 = automatic
  --monitor-port PORT HTTP capture port (default automatic)
  --latency 20..500    Jitter buffer in milliseconds (default 60)
  --playback DEVICE   Sound card output: default, index or exact device name
  --playback-map LIST Physical output channels, e.g. 1,2 or 1,2,3,4
  --volume 0..100     Hardware playback volume in percent (default 50)
Controller/demo/hardware options:
  --host IPV4          HTTP bind address; use 0.0.0.0 for trusted LAN access
  --state FILE         Route persistence (default .openaoip/routes.json)
  --web-dir DIR        Browser assets (default web next to executable)
Hardware mode:
  Starts a controller, microphone sender and sound card receiver (1 stereo pair).
  --streams/--inputs and channel maps configure multichannel sound cards.
  No initial routes: connect in the browser when ready. Use headphones locally.
All modes:
  --duration SECONDS   Stop automatically (default run until Ctrl+C)

Transport: RTP v2, dynamic PT96, L16/48000/2, 5 ms packets, IPv4 unicast.
Hardware capture uses its device clock; playback has a bounded software buffer.
PTP synchronization and AES67 interoperability are not implemented.
)";
}
Options parse(int argc, char** argv, const std::string& command) {
  std::set<std::string> allowed = {"--duration", "--host"};
  if (command == "controller" || command == "demo" || command == "hardware")
    allowed.insert({"--port", "--state", "--web-dir"});
  else if (command == "sender")
    allowed.insert({"--id", "--name", "--controller", "--streams", "--tone", "--wav", "--capture",
                    "--capture-map"});
  else if (command == "receiver")
    allowed.insert({"--id", "--name", "--controller", "--inputs", "--port", "--monitor-port",
                    "--latency", "--playback", "--playback-map", "--volume"});
  else if (command == "audio-devices")
    allowed = {"--json"};
  else if (command == "device")
    allowed.insert({"--config", "--id", "--name", "--controller", "--streams", "--inputs",
                    "--capture", "--capture-map", "--playback", "--playback-map", "--volume",
                    "--latency", "--port", "--monitor-port", "--tone", "--wav"});
  else
    throw std::runtime_error("Unknown command: " + command);
  if (command == "hardware")
    allowed.insert({"--capture", "--capture-map", "--playback", "--playback-map", "--streams",
                    "--inputs", "--volume", "--latency"});
  Options options;
  for (int i = 2; i < argc; ++i) {
    const std::string key = argv[i];
    if (!allowed.contains(key))
      throw std::runtime_error("Unknown option: " + key);
    if (key == "--json") {
      options.values[key].push_back("true");
      continue;
    }
    if (++i == argc || std::string(argv[i]).starts_with("--"))
      throw std::runtime_error("Missing value for " + key);
    if (options.values.contains(key) && key != "--wav")
      throw std::runtime_error("Duplicate option: " + key);
    options.values[key].push_back(argv[i]);
  }
  return options;
}
void wait(int seconds) {
  const auto start = std::chrono::steady_clock::now();
  while (!stopping &&
         (!seconds || std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds)))
    std::this_thread::sleep_for(50ms);
}
} // namespace
int main(int argc, char** argv) {
  try {
    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "help") {
      help();
      return 0;
    }
    for (int i = 2; i < argc; ++i)
      if (std::string(argv[i]) == "--help") {
        help();
        return 0;
      }
    const std::string command = argv[1];
    const auto parsed = parse(argc, argv, command);
    const auto options = command == "device" ? device_options(parsed) : parsed;
    const int duration = options.number("--duration", 0, 0, 86400);
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    aoip::NetworkRuntime network;
    if (command == "audio-devices") {
      const auto devices = aoip::audio_devices();
      if (options.values.contains("--json")) {
        aoip::Json result = {{"backend", devices.backend},
                             {"capture", aoip::Json::array()},
                             {"playback", aoip::Json::array()}};
        for (const auto& d : devices.capture)
          result["capture"].push_back(
              {{"index", d.index}, {"name", d.name}, {"default", d.is_default}});
        for (const auto& d : devices.playback)
          result["playback"].push_back(
              {{"index", d.index}, {"name", d.name}, {"default", d.is_default}});
        std::cout << result.dump(2) << std::endl;
      } else {
        std::cout << "Audio backend: " << devices.backend << "\n\nCapture / microphones:\n";
        for (const auto& d : devices.capture)
          std::cout << "  " << d.index << "  " << d.name << (d.is_default ? " [default]" : "")
                    << '\n';
        std::cout << "\nPlayback / speakers:\n";
        for (const auto& d : devices.playback)
          std::cout << "  " << d.index << "  " << d.name << (d.is_default ? " [default]" : "")
                    << '\n';
        std::cout
            << "\nUse --capture or --playback with default, an index or an exact device name.\n";
      }
    } else if (command == "device") {
      if (!options.values.contains("--id"))
        throw std::runtime_error("Device mode requires a unique --id or an id in --config");
      if (options.values.contains("--capture") &&
          (options.values.contains("--tone") || options.values.contains("--wav")))
        throw std::runtime_error("Choose physical capture OR tone/WAV sources");
      if (options.values.contains("--tone") && options.values.contains("--wav"))
        throw std::runtime_error("Choose --tone OR --wav");
      aoip::DeviceConfig config;
      config.id = options.get("--id", "");
      config.name = options.get("--name", config.id);
      config.controller = options.get("--controller", config.controller);
      config.host = options.get("--host", config.host);
      config.sender.streams = options.number("--streams", 1, 0, 16);
      config.receiver.inputs = options.number("--inputs", 1, 0, 16);
      const bool generated = options.values.contains("--tone") || options.values.contains("--wav");
      config.sender.capture_device = generated ? "" : options.get("--capture", "default");
      config.sender.capture_channels = mapping(options, "--capture-map");
      config.sender.tone = options.number("--tone", 440, 20, 18000);
      if (options.values.contains("--wav"))
        config.sender.wav = options.values.at("--wav");
      config.receiver.playback_device = options.get("--playback", "default");
      if (config.receiver.playback_device == "off")
        config.receiver.playback_device.clear();
      config.receiver.playback_channels = mapping(options, "--playback-map");
      config.receiver.volume = options.number("--volume", 50, 0, 100) / 100.0f;
      config.receiver.latency_ms = options.number("--latency", 60, 20, 500);
      config.receiver.port = options.number("--port", 5004, 0, 65535);
      config.receiver.monitor_port = options.number("--monitor-port", 5100, 0, 65535);
      aoip::NetworkDevice device(config);
      std::cout << "Device " << config.name << " (" << config.id << "): " << config.sender.streams
                << " send streams, " << config.receiver.inputs << " receive inputs -> "
                << config.controller
                << "\nRegistering automatically; connect routes in the controller. Ctrl+C to stop."
                << std::endl;
      wait(duration);
    } else if (command == "sender") {
      aoip::SenderConfig config;
      config.id = options.get("--id", config.id);
      config.name = options.get("--name", config.id);
      config.controller = options.get("--controller", config.controller);
      config.host = options.get("--host", config.host);
      config.capture_device = options.get("--capture", "");
      config.capture_channels = mapping(options, "--capture-map");
      config.streams = options.number("--streams", config.capture_device.empty() ? 2 : 1, 1, 16);
      config.tone = options.number("--tone", 440, 20, 18000);
      if (options.values.contains("--wav"))
        config.wav = options.values.at("--wav");
      aoip::Sender sender(config);
      std::cout << "Sender " << config.id << ": " << config.streams << " stereo streams -> "
                << config.controller << std::endl;
      wait(duration);
    } else if (command == "receiver") {
      aoip::ReceiverConfig config;
      config.id = options.get("--id", config.id);
      config.name = options.get("--name", config.id);
      config.controller = options.get("--controller", config.controller);
      config.host = options.get("--host", config.host);
      config.playback_device = options.get("--playback", "");
      config.playback_channels = mapping(options, "--playback-map");
      config.volume = options.number("--volume", 50, 0, 100) / 100.0f;
      config.inputs = options.number("--inputs", config.playback_device.empty() ? 2 : 1, 1, 16);
      config.port = options.number("--port", 5004, 0, 65535);
      config.monitor_port = options.number("--monitor-port", 0, 0, 65535);
      config.latency_ms = options.number("--latency", 60, 20, 500);
      aoip::Receiver receiver(config);
      std::cout << "Receiver " << config.id << ": " << config.inputs << " stereo inputs -> "
                << config.controller << std::endl;
      wait(duration);
    } else {
      aoip::ControllerConfig config;
      config.host = options.get("--host", config.host);
      config.port = options.number("--port", 3210, 0, 65535);
      config.state_file = options.get(
          "--state", command == "hardware" ? ".openaoip/hardware-routes.json" : config.state_file);
      const auto sibling_web = std::filesystem::absolute(argv[0]).parent_path() / "web";
      config.web_root = options.get(
          "--web-dir", std::filesystem::is_directory(sibling_web) ? sibling_web.string() : "web");
      aoip::Controller controller(config);
      const auto address = "http://127.0.0.1:" + std::to_string(controller.port());
      std::cout << "OpenAoIP controller: http://"
                << (config.host == "0.0.0.0" ? "127.0.0.1" : config.host) << ":"
                << controller.port() << std::endl;
      for (const auto& ip : aoip::local_addresses())
        if (config.host == "0.0.0.0" || config.host == ip)
          std::cout << "LAN controller: http://" << ip << ':' << controller.port() << std::endl;
      if (command == "hardware") {
        const auto local = "http://" + aoip::local_control_host(config.host) + ":" +
                           std::to_string(controller.port());
        aoip::SenderConfig s;
        s.id = "hardware-tx";
        s.name = "Microphone / Sound card";
        s.controller = local;
        s.host = config.host;
        s.capture_device = options.get("--capture", "default");
        s.capture_channels = mapping(options, "--capture-map");
        s.streams = options.number("--streams", 1, 1, 16);
        aoip::ReceiverConfig r;
        r.id = "hardware-rx";
        r.name = "Sound card output";
        r.controller = local;
        r.host = config.host;
        r.port = 0;
        r.playback_device = options.get("--playback", "default");
        r.playback_channels = mapping(options, "--playback-map");
        r.inputs = options.number("--inputs", 1, 1, 16);
        r.volume = options.number("--volume", 50, 0, 100) / 100.0f;
        r.latency_ms = options.number("--latency", 60, 20, 500);
        aoip::Sender sender(s);
        aoip::Receiver receiver(r);
        std::cout << "Hardware audio started. Route microphone inputs to sound card outputs in the "
                     "browser.\nUse headphones when capturing and playing on the same computer. "
                     "Ctrl+C to stop."
                  << std::endl;
        wait(duration);
      } else if (command == "demo") {
        // Independent endpoint instances exchange actual UDP packets, even in the one-process demo.
        std::vector<std::unique_ptr<aoip::Sender>> senders;
        std::vector<std::unique_ptr<aoip::Receiver>> receivers;
        const auto local = config.host == "0.0.0.0"
                               ? address
                               : "http://" + config.host + ":" + std::to_string(controller.port());
        for (int i = 0; i < 2; ++i) {
          aoip::SenderConfig s;
          s.id = "demo-tx-" + std::to_string(i + 1);
          s.name = i == 0 ? "Stage rack" : "Playback rack";
          s.controller = local;
          s.host = "127.0.0.1";
          s.tone = i == 0 ? 440 : 660;
          senders.push_back(std::make_unique<aoip::Sender>(s));
          aoip::ReceiverConfig r;
          r.id = "demo-rx-" + std::to_string(i + 1);
          r.name = i == 0 ? "Front of house" : "Monitor rack";
          r.controller = local;
          r.port = 0;
          r.host = "127.0.0.1";
          receivers.push_back(std::make_unique<aoip::Receiver>(r));
        }
        bool registered = false;
        for (int i = 0; i < 100 && !stopping; ++i) {
          if (controller.state().at("devices").size() == 4) {
            registered = true;
            break;
          }
          std::this_thread::sleep_for(50ms);
        }
        if (!registered && !stopping)
          throw std::runtime_error("Demo devices did not register");
        if (registered && controller.fresh())
          for (int device = 1; device <= 2; ++device)
            for (int stream = 1; stream <= 2; ++stream)
              controller.set_route({{"sender", "demo-tx-" + std::to_string(device)},
                                    {"stream", "stream-" + std::to_string(stream)},
                                    {"receiver", "demo-rx-" + std::to_string(device)},
                                    {"input", "input-" + std::to_string(stream)}});
        std::cout << "Demo: 2 senders, 2 receivers, 4 independent stereo streams. Open the "
                     "controller to route and audition.\nCtrl+C to stop."
                  << std::endl;
        wait(duration);
      } else
        wait(duration);
    }
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << '\n';
    return 1;
  }
}
