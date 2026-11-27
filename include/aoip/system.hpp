#pragma once
#include "aoip/audio_io.hpp"
#include "json.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace aoip {
using Json = nlohmann::json;
struct ControllerConfig {
  std::string host = "127.0.0.1";
  int port = 3210;
  std::string state_file = ".openaoip/routes.json", web_root = "web";
};
class Controller {
public:
  explicit Controller(ControllerConfig config = {});
  ~Controller();
  Controller(const Controller&) = delete;
  Controller& operator=(const Controller&) = delete;
  int port() const;
  Json state() const;
  void set_route(const Json& route);
  void remove_route(const std::string& receiver, const std::string& input, int channel = 0);
  void set_device_name(const std::string& id, const std::string& name);
  void set_channel_name(const std::string& id, const std::string& direction,
                        const std::string& endpoint, int side, const std::string& name);
  Json preset() const;
  void restore_preset(const Json&);
  bool fresh() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
struct SenderConfig {
  std::string id = "sender-1", name = "Sender 1", controller = "http://127.0.0.1:3210",
              host = "0.0.0.0";
  int streams = 2;
  double tone = 440;
  std::vector<std::string> wav;
  std::string capture_device;
  std::vector<int> capture_channels;
  AudioBackend audio_backend = AudioBackend::System;
  bool register_with_controller = true;
  int rate = 48000;
  std::function<bool(std::span<Pcm>)> external_capture;
  // Optional software audio clock, in local monotonic milliseconds per 240-frame block.
  std::function<double()> packet_interval;
};
class Sender {
public:
  explicit Sender(SenderConfig config = {});
  ~Sender();
  Sender(const Sender&) = delete;
  Sender& operator=(const Sender&) = delete;
  Json describe() const;
  void apply_plan(const Json& plan);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
struct ReceiverConfig {
  std::string id = "receiver-1", name = "Receiver 1", controller = "http://127.0.0.1:3210",
              host = "0.0.0.0";
  int inputs = 2, port = 5004, monitor_port = 0, latency_ms = 60;
  std::string playback_device;
  std::vector<int> playback_channels;
  float volume = 0.5f;
  AudioBackend audio_backend = AudioBackend::System;
  bool register_with_controller = true;
  int rate = 48000;
  std::function<void(std::span<const Pcm>)> external_playback;
  std::function<double()> packet_interval;
};
class Receiver {
public:
  explicit Receiver(ReceiverConfig config = {});
  ~Receiver();
  Receiver(const Receiver&) = delete;
  Receiver& operator=(const Receiver&) = delete;
  Json describe() const;
  void apply_plan(const Json& plan);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
struct DeviceConfig {
  std::string id = "device-1", name = "Device 1", controller = "http://127.0.0.1:3210",
              host = "0.0.0.0";
  SenderConfig sender;
  ReceiverConfig receiver;
  int tx_channels = 0, rx_channels = 0;
  std::function<void(const Json&)> on_plan;
  std::function<void(const std::string&)> on_error;
  std::vector<std::string> tx_names, rx_names;
  std::function<Json()> clock_status;
  bool register_with_controller = true;
};
// One registration and one identity, with independent send and receive workers.
// Set sender.streams or receiver.inputs to zero to disable that direction.
class NetworkDevice {
public:
  explicit NetworkDevice(DeviceConfig config);
  ~NetworkDevice();
  NetworkDevice(const NetworkDevice&) = delete;
  NetworkDevice& operator=(const NetworkDevice&) = delete;
  Json describe() const;
  void apply_plan(const Json&);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
} // namespace aoip
