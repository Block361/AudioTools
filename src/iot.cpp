// SPDX-License-Identifier: MIT
#include "aoip/iot.hpp"
#include "aoip/config_store.hpp"
#include "aoip/discovery.hpp"
#include "aoip/network.hpp"
#include "aoip/version.hpp"
#include "httplib.h"
#include <atomic>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#ifdef _WIN32
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/random.h>
#include <unistd.h>
#endif
namespace aoip::iot {
namespace {
void keys(const Json& value, std::initializer_list<const char*> allowed) {
  if (!value.is_object())
    throw std::runtime_error("Expected a JSON object");
  const std::set<std::string> valid(allowed.begin(), allowed.end());
  for (const auto& [key, item] : value.items())
    if (!valid.contains(key))
      throw std::runtime_error("Unknown configuration field: " + key);
}
void validate_audio(const bridge::Settings& audio, bool required) {
  bridge::validate(audio, required);
  for (const auto& e : audio.endpoints)
    if (e.name.empty() || e.name.size() > 160 || e.device.size() > 1024)
      throw std::runtime_error("Invalid IoT endpoint label or device identifier");
  for (const auto& e : audio.endpoints)
    if (e.mode == EndpointMode::Loopback)
      throw std::runtime_error(
          "IoT uses ALSA capture/playback. For loopback select an ALSA loopback capture device.");
}
bridge::Settings parse_audio(const Json& value) {
  keys(value, {"id", "name", "controller", "interface", "txChannels", "rxChannels", "sampleRate",
               "bufferFrames", "latencyMs", "localLatencyMs", "clockSource", "ptpDomain", "txNames",
               "rxNames", "endpoints"});
  for (const auto* key : {"txChannels", "rxChannels", "sampleRate", "bufferFrames", "latencyMs",
                          "localLatencyMs", "ptpDomain"})
    if (value.contains(key) &&
        (!value.at(key).is_number_integer() || value.at(key).get<std::int64_t>() < 0 ||
         value.at(key).get<std::int64_t>() > 192000))
      throw std::runtime_error("Invalid audio integer: " + std::string(key));
  const auto endpoints = value.value("endpoints", Json::array());
  if (!endpoints.is_array() || endpoints.size() > 32)
    throw std::runtime_error("At most 32 audio endpoints are allowed");
  for (const auto& e : endpoints) {
    keys(e, {"id", "name", "device", "mode", "patches"});
    const auto patches = e.at("patches");
    if (!patches.is_array() || patches.size() > 32)
      throw std::runtime_error("At most 32 patches per endpoint are allowed");
    for (const auto& p : patches) {
      keys(p, {"local", "network", "gain"});
      for (const auto* k : {"local", "network"})
        if (!p.at(k).is_number_integer() || p.at(k).get<std::int64_t>() < 1 ||
            p.at(k).get<std::int64_t>() > 32)
          throw std::runtime_error("Patch channels must be integers between 1 and 32");
    }
  }
  return bridge::from_json(value);
}
void reply(httplib::Response& r, const Json& j, int code = 200) {
  r.status = code;
  r.set_header("Cache-Control", "no-store");
  r.set_content(j.dump(), "application/json");
}
bool same_key(const std::string& actual, const std::string& expected) {
  unsigned diff = unsigned(actual.size() ^ expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i)
    diff |= unsigned(expected[i] ^ (i < actual.size() ? actual[i] : 0));
  return diff == 0;
}
struct Owner {
#ifdef _WIN32
  HANDLE handle = INVALID_HANDLE_VALUE;
  explicit Owner(const std::filesystem::path& path) {
    auto lock = path;
    lock += L".lock";
    handle = CreateFileW(lock.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
      throw std::runtime_error("Another IoT process owns this configuration");
  }
  ~Owner() {
    if (handle != INVALID_HANDLE_VALUE)
      CloseHandle(handle);
  }
#else
  int fd = -1;
  explicit Owner(const std::filesystem::path& path) {
    auto lock = path;
    lock += ".lock";
    fd = ::open(lock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
      throw std::runtime_error("Cannot open the IoT instance lock");
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
      ::close(fd);
      fd = -1;
      throw std::runtime_error("Another IoT process owns this configuration");
    }
  }
  ~Owner() {
    if (fd >= 0)
      ::close(fd);
  }
#endif
};
} // namespace
Configuration initial_configuration() {
  Configuration c;
  unsigned char bytes[32];
#ifdef _WIN32
  if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
    throw std::runtime_error("Cannot generate an IoT management key");
#else
  std::size_t at = 0;
  while (at < sizeof(bytes)) {
    const auto n = getrandom(bytes + at, sizeof(bytes) - at, 0);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      throw std::runtime_error("Cannot generate an IoT management key");
    at += std::size_t(n);
  }
#endif
  std::ostringstream key;
  for (const auto b : bytes)
    key << std::hex << std::setw(2) << std::setfill('0') << int(b);
  c.token = key.str();
  c.audio.id = "iot-" + random_id();
  c.audio.name = "OpenAoIP IoT";
  c.audio.tx_channels = c.audio.rx_channels = 2;
  c.audio.buffer_frames = 512;
  c.audio.latency_ms = 80;
  c.audio.endpoints = {
      {"output", "Local output", "default", EndpointMode::Playback, {{1, 1, 1}, {2, 2, 1}}}};
  return c;
}
Configuration configuration_from_json(const Json& j) {
  keys(j, {"product", "schemaVersion", "revision", "autoStart", "management", "audio"});
  if (j.at("product") != "OpenAoIP IoT" || j.at("schemaVersion") != protocol_version)
    throw std::runtime_error("Unsupported OpenAoIP IoT configuration schema");
  Configuration c;
  c.audio = parse_audio(j.at("audio"));
  c.auto_start = j.at("autoStart").get<bool>();
  c.revision = j.at("revision").get<std::uint64_t>();
  const auto& m = j.at("management");
  keys(m, {"bind", "port", "token"});
  if (!m.at("port").is_number_integer() || m.at("port").get<std::int64_t>() < 0 ||
      m.at("port").get<std::int64_t>() > 65535)
    throw std::runtime_error(
        "Management port must be an integer between 1024 and 65535, or 0 for automatic selection");
  c.bind = m.at("bind");
  c.port = m.at("port").get<int>();
  c.token = m.at("token");
  if (!valid_ipv4(c.bind) || c.port < 0 || c.port > 65535 || (c.port && c.port < 1024) ||
      c.token.size() != 64 || c.token.find_first_not_of("0123456789abcdef") != std::string::npos ||
      !c.revision || c.revision > 9007199254740991ULL)
    throw std::runtime_error("Invalid IoT bind address, port, management key or revision");
  validate_audio(c.audio, c.auto_start);
  return c;
}
Json to_json(const Configuration& c, bool secret) {
  Json j = {{"product", "OpenAoIP IoT"},
            {"schemaVersion", protocol_version},
            {"revision", c.revision},
            {"autoStart", c.auto_start},
            {"audio", bridge::to_json(c.audio)}};
  if (secret)
    j["management"] = {{"bind", c.bind}, {"port", c.port}, {"token", c.token}};
  else
    j["id"] = c.audio.id;
  return j;
}
Json audio_inventory(AudioBackend backend) {
  const auto devices = audio_devices(backend);
  Json j = {{"backend", devices.backend},
            {"loopbackSupported", false},
            {"capture", Json::array()},
            {"playback", Json::array()},
            {"interfaces", local_addresses()}};
  for (const auto& d : devices.capture)
    j["capture"].push_back({{"id", d.id}, {"name", d.name}, {"default", d.is_default}});
  for (const auto& d : devices.playback)
    j["playback"].push_back({{"id", d.id}, {"name", d.name}, {"default", d.is_default}});
  return j;
}
struct Host::Impl {
  std::filesystem::path file;
  Owner owner;
  Configuration config;
  const std::string management_key;
  AudioBackend backend;
  httplib::Server server;
  std::thread http, heart;
  mutable std::mutex mutex;
  std::unique_ptr<bridge::Engine> engine;
  std::atomic<bool> alive{true};
  std::string instance = random_id(), controller, last_error, start_error;
  double last_plan = 0;
  int port = 0;
  Impl(std::filesystem::path path, AudioBackend b)
      : file(std::filesystem::absolute(std::move(path))), owner(file),
        config(configuration_from_json(read_configuration(file, 128 * 1024))),
        management_key(config.token), backend(b) {
    server.set_payload_max_length(128 * 1024);
    server.set_read_timeout(3, 0);
    server.set_write_timeout(3, 0);
    server.set_pre_routing_handler([this](const auto& req, auto& res) {
      if (req.has_header("Origin") ||
          !same_key(req.get_header_value("Authorization"), "Bearer " + management_key)) {
        reply(res, {{"error", "IoT management key is missing or invalid"}}, 403);
        return httplib::Server::HandlerResponse::Handled;
      }
      return httplib::Server::HandlerResponse::Unhandled;
    });
    server.set_exception_handler([](const auto&, auto& res, std::exception_ptr e) {
      try {
        std::rethrow_exception(e);
      } catch (const std::exception& ex) {
        reply(res, {{"error", ex.what()}}, 400);
      }
    });
    server.Get("/v1/config", [this](const auto&, auto& res) {
      std::lock_guard lock(mutex);
      reply(res, stamp(to_json(config)));
    });
    server.Get("/v1/status", [this](const auto&, auto& res) { reply(res, status()); });
    server.Get("/v1/audio-devices", [this](const auto&, auto& res) {
      auto j = audio_inventory(backend);
      std::lock_guard lock(mutex);
      reply(res, stamp(j));
    });
    server.Post("/v1/config", [this](const auto& req, auto& res) {
      const auto body = Json::parse(req.body);
      keys(body, {"revision", "audio"});
      auto next = parse_audio(body.at("audio"));
      std::lock_guard lock(mutex);
      if (!revision_matches(body, res))
        return;
      if (next.id != config.audio.id || next.controller != config.audio.controller)
        throw std::runtime_error(
            "Device ID and controller bootstrap address are local-file settings");
      const auto addresses=local_addresses();
      if(next.interface_ip!="0.0.0.0" && !next.interface_ip.starts_with("127.") &&
         std::find(addresses.begin(),addresses.end(),next.interface_ip)==addresses.end())
        throw std::runtime_error("The selected IPv4 address is not assigned to this IoT device");
      validate_audio(next, bool(engine));
      auto updated = config;
      updated.audio = std::move(next);
      ++updated.revision;
      replace(updated);
      reply(res, stamp(to_json(config)));
    });
    server.Post("/v1/run", [this](const auto& req, auto& res) {
      const auto body = Json::parse(req.body);
      keys(body, {"revision", "running"});
      std::lock_guard lock(mutex);
      if (!revision_matches(body, res))
        return;
      auto updated = config;
      updated.auto_start = body.at("running").template get<bool>();
      ++updated.revision;
      validate_audio(updated.audio, updated.auto_start);
      replace(updated);
      reply(res, stamp(to_json(config)));
    });
    port = config.port ? config.port : server.bind_to_any_port(config.bind);
    if ((config.port && !server.bind_to_port(config.bind, port)) || port <= 0)
      throw std::runtime_error("Cannot bind the IoT management port");
    if (config.auto_start) {
      try {
        engine = make_engine(config.audio);
      } catch (const std::exception& e) {
        start_error = e.what();
      } // Remain manageable after audio-start failure.
    }
  }
  void start() {
    http = std::thread([this] { server.listen_after_bind(); });
    try {
      heart = std::thread([this] { registration(); });
    } catch (...) {
      server.stop();
      http.join();
      throw;
    }
  }
  ~Impl() {
    alive = false;
    server.stop();
    if (http.joinable())
      http.join();
    if (heart.joinable())
      heart.join();
    engine.reset();
  }
  Json stamp(Json j) const {
    j["product"] = "OpenAoIP IoT";
    j["version"] = version;
    j["schemaVersion"] = protocol_version;
    j["id"] = config.audio.id;
    return j;
  }
  bool revision_matches(const Json& body, httplib::Response& res) const {
    if (!body.at("revision").is_number_integer() || body.at("revision") != config.revision) {
      reply(res, {{"error", "Configuration changed on the device; refresh before applying"}}, 409);
      return false;
    }
    if (config.revision >= 9007199254740991ULL)
      throw std::runtime_error("IoT configuration revision limit reached");
    return true;
  }
  std::unique_ptr<bridge::Engine> make_engine(bridge::Settings audio) {
    if (!controller.empty())
      audio.controller = controller;
    return std::make_unique<bridge::Engine>(std::move(audio), backend, nullptr, false);
  }
  void replace(const Configuration& updated) {
    const bool was_running = bool(engine);
    auto previous = config;
    engine
        .reset(); // Endpoint and PTP owners must be released before opening the new configuration.
    try {
      if (updated.auto_start)
        engine = make_engine(updated.audio);
      write_configuration(file, to_json(updated, true));
      config = updated;
      last_error.clear();
      start_error.clear();
    } catch (const std::exception& e) {
      const std::string failure = e.what();
      engine.reset();
      try {
        if (was_running)
          engine = make_engine(previous.audio);
      } catch (const std::exception& rollback) {
        last_error = "Rollback could not restart audio: " + std::string(rollback.what());
      }
      throw std::runtime_error("IoT changes were not saved: " + failure);
    }
  }
  Json status() const {
    std::lock_guard lock(mutex);
    auto j = engine ? engine->status() : Json{{"running", false}, {"state", "Stopped"}};
    j["revision"] = config.revision;
    j["autoStart"] = config.auto_start;
    j["managementPort"] = port;
    j["controllerConnected"] = last_plan > 0 && monotonic_ms() - last_plan < 2500;
    j["controller"] = controller;
    j["managementError"] = last_error;
    j["startError"] = start_error;
    return stamp(std::move(j));
  }
  Json describe() const {
    Json d;
    const auto live = engine ? engine->status() : Json{{"running", false}, {"state", "Stopped"}};
    if (engine)
      d = live.value("device", Json());
    if (d.is_null() || d.empty())
      d = {{"streams", Json::array()}, {"inputs", Json::array()}};
    d["id"] = config.audio.id;
    d["name"] = config.audio.name;
    d["role"] = "device";
    d["instance"] = instance;
    d["format"] = {{"sampleRate", config.audio.rate}, {"bits", 16}, {"channels", 2}};
    d["management"] = {{"product", "OpenAoIP IoT"},
                       {"apiVersion", protocol_version},
                       {"port", port},
                       {"running", live.value("running", false)},
                       {"revision", config.revision}};
    return d;
  }
  void registration() {
    while (alive) {
      try {
        std::string target, bind;
        Json d;
        std::uint64_t generation;
        {
          std::lock_guard lock(mutex);
          bind = config.audio.interface_ip;
          target = config.audio.controller;
        }
        if (target == "auto") {
          {
            std::lock_guard lock(mutex);
            target = controller;
          }
          if (target.empty()) {
            const auto list = discover_controllers(bind, 300);
            if (!list.empty())
              target = list.front().url;
          }
        }
        if (target.empty() || target == "auto")
          throw std::runtime_error("Waiting for a controller; management remains available");
        {
          std::lock_guard lock(mutex);
          controller = target;
          d = describe();
          generation = config.revision;
        }
        httplib::Client client(target);
        if (bind != "0.0.0.0")
          client.set_interface(bind);
        client.set_connection_timeout(1, 0);
        client.set_read_timeout(1, 0);
        client.set_write_timeout(1, 0);
        const auto result = client.Post("/api/heartbeat", d.dump(), "application/json");
        if (!result)
          throw std::runtime_error("Controller unreachable; existing audio routes continue");
        const auto plan = Json::parse(result->body);
        if (result->status != 200)
          throw std::runtime_error(plan.value("error", "IoT registration rejected"));
        {
          std::lock_guard lock(mutex);
          if (generation == config.revision) {
            if (engine)
              engine->apply_plan(plan);
            last_plan = monotonic_ms();
            last_error.clear();
          }
        }
      } catch (const std::exception& e) {
        std::lock_guard lock(mutex);
        last_error = e.what();
      }
      for (int n = 0; n < 10 && alive; ++n)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }
};
Host::Host(std::filesystem::path file, AudioBackend backend)
    : impl_(std::make_unique<Impl>(std::move(file), backend)) {
  impl_->start();
}
Host::~Host() = default;
int Host::port() const {
  return impl_->port;
}
Json Host::status() const {
  return impl_->status();
}
} // namespace aoip::iot
