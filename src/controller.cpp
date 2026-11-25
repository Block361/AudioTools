#include "aoip/audio.hpp"
#include "aoip/network.hpp"
#include "aoip/system.hpp"
#include "httplib.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <set>

namespace aoip {
namespace {
std::int64_t epoch_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
void reply(httplib::Response& res, const Json& body, int code = 200) {
  res.status = code;
  res.set_header("Cache-Control", "no-store");
  res.set_content(body.dump(), "application/json");
}
std::string route_key(const Json& r) {
  return r.at("receiver").get<std::string>() + ":" + r.at("input").get<std::string>() +
         (r.contains("receiverChannel") ? ":" + std::to_string(r.at("receiverChannel").get<int>())
                                        : "");
}
void validate_route(const Json& route) {
  if (!route.is_object())
    throw std::runtime_error("Invalid route");
  for (const auto* field : {"sender", "stream", "receiver", "input"})
    if (!route.contains(field) || !route[field].is_string() ||
        !valid_id(route[field].get<std::string>()))
      throw std::runtime_error("Invalid route identity");
  if (route.contains("receiverChannel") != route.contains("senderChannel"))
    throw std::runtime_error("Select both source and destination channel");
  for (const auto* field : {"senderChannel", "receiverChannel"})
    if (route.contains(field) && (!route[field].is_number_integer() ||
                                  route[field].get<int>() < 1 || route[field].get<int>() > 2))
      throw std::runtime_error("Channel must be 1 or 2 inside a stream");
}
void split_stereo_route(std::map<std::string, Json>& routes, const std::string& receiver,
                        const std::string& input) {
  const auto base = receiver + ":" + input;
  const auto it = routes.find(base);
  if (it == routes.end())
    return;
  const auto original = it->second;
  routes.erase(it);
  for (int side = 1; side <= 2; ++side) {
    auto r = original;
    r["senderChannel"] = side;
    r["receiverChannel"] = side;
    routes[route_key(r)] = r;
  }
}
bool is_online(const Json& d) {
  return epoch_ms() - d.at("lastSeen").get<std::int64_t>() < 3500;
}
Json find_channel(const Json& device, const char* field, const std::string& id) {
  for (const auto& c : device.at(field))
    if (c.at("id") == id)
      return c;
  return nullptr;
}
bool route_channels_available(const Json& route, const Json& stream, const Json& input) {
  if (stream.is_null() || input.is_null()) return false;
  if (route.contains("senderChannel"))
    return route.at("senderChannel").get<int>() <= stream.value("channels", 2) &&
           route.at("receiverChannel").get<int>() <= input.value("channels", 2);
  return stream.value("channels", 2) >= input.value("channels", 2);
}
int bounded_int(const Json& object, const char* key, int low, int high) {
  if (!object.contains(key) || !object[key].is_number_integer())
    throw std::runtime_error(std::string("Invalid ") + key);
  const auto n = object[key].get<std::int64_t>();
  if (n < low || n > high)
    throw std::runtime_error(std::string("Out of range: ") + key);
  return static_cast<int>(n);
}
void write_atomic(const std::string& file, const Json& data) {
  const auto path = std::filesystem::absolute(file),
             temporary = std::filesystem::path(path.string() + ".tmp");
  std::filesystem::create_directories(path.parent_path());
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out.exceptions(std::ios::failbit | std::ios::badbit);
    out << data.dump(2) << '\n';
    out.close();
  }
#ifdef _WIN32
  if (!MoveFileExW(temporary.c_str(), path.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    throw std::runtime_error("Cannot replace route state file");
#else
  std::filesystem::rename(temporary, path);
#endif
}
} // namespace
struct Controller::Impl {
  ControllerConfig config;
  httplib::Server server;
  std::thread thread;
  mutable std::mutex mutex;
  std::map<std::string, Json> devices, routes;
  Json device_names = Json::object();
  Json channel_names = Json::object();
  std::mutex hardware_mutex;
  std::unique_ptr<Sender> local_sender;
  std::unique_ptr<Receiver> local_receiver;
  std::atomic<bool> local_audio_active{false};
  Json local_audio_config = nullptr;
  Json events = Json::array();
  std::uint64_t revision = 0;
  int port = 0;
  bool fresh = true;
  explicit Impl(ControllerConfig c) : config(std::move(c)) {
    if (!valid_ipv4(config.host) || config.port < 0 || config.port > 65535)
      throw std::runtime_error("Invalid controller bind address or port");
    if (!config.state_file.empty() && std::filesystem::exists(config.state_file)) {
      fresh = false;
      std::ifstream in(config.state_file);
      Json saved;
      in >> saved;
      device_names = saved.value("deviceNames", Json::object());
      channel_names = saved.value("channelNames", Json::object());
      if (saved.at("version") != 1 || !saved.at("routes").is_array())
        throw std::runtime_error("Unsupported route state file");
      for (const auto& r : saved.at("routes")) {
        validate_route(r);
        routes[route_key(r)] = r;
      }
    }
    server.set_payload_max_length(128 * 1024);
    server.set_read_timeout(3, 0);
    server.set_write_timeout(3, 0);
    server.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
      if (req.has_header("Origin") &&
          req.get_header_value("Origin") != "http://" + req.get_header_value("Host")) {
        reply(res, {{"error", "Cross-origin requests are disabled"}}, 403);
        return httplib::Server::HandlerResponse::Handled;
      }
      return httplib::Server::HandlerResponse::Unhandled;
    });
    server.set_exception_handler([](const auto&, auto& res, std::exception_ptr ep) {
      try {
        std::rethrow_exception(ep);
      } catch (const std::exception& e) {
        reply(res, {{"error", e.what()}}, 400);
      } catch (...) {
        reply(res, {{"error", "Internal error"}}, 500);
      }
    });
    server.Get("/api/state", [this](const auto&, auto& res) {
      std::lock_guard lock(mutex);
      reply(res, state_locked());
    });
    server.Get("/api/network", [this](const auto&, auto& res) {
      Json addresses = Json::array();
      for (const auto& ip : local_addresses())
        addresses.push_back({{"address", ip},
                             {"controllerUrl", "http://" + ip + ":" + std::to_string(port)},
                             {"reachable", config.host == "0.0.0.0" || config.host == ip}});
      reply(res, {{"bindHost", config.host},
                  {"port", port},
                  {"addresses", addresses},
                  {"loopbackUrl", "http://127.0.0.1:" + std::to_string(port)}});
    });
    server.Get("/api/audio-devices", [](const auto& req, auto& res) {
      if (req.remote_addr != "127.0.0.1" && req.remote_addr != req.local_addr)
        return reply(
            res, {{"error", "Hardware setup is available on the controller computer only"}}, 403);
      const auto list = audio_devices();
      Json result = {
          {"backend", list.backend}, {"capture", Json::array()}, {"playback", Json::array()}};
      for (const auto& d : list.capture)
        result["capture"].push_back(
            {{"index", d.index}, {"name", d.name}, {"default", d.is_default}});
      for (const auto& d : list.playback)
        result["playback"].push_back(
            {{"index", d.index}, {"name", d.name}, {"default", d.is_default}});
      reply(res, result);
    });
    server.Post("/api/hardware", [this](const auto& req, auto& res) {
      if (req.remote_addr != "127.0.0.1" && req.remote_addr != req.local_addr)
        return reply(
            res, {{"error", "Hardware setup is available on the controller computer only"}}, 403);
      start_hardware(Json::parse(req.body));
      reply(res, {{"ok", true}});
    });
    server.Delete("/api/hardware", [this](const auto& req, auto& res) {
      if (req.remote_addr != "127.0.0.1" && req.remote_addr != req.local_addr)
        return reply(
            res, {{"error", "Hardware setup is available on the controller computer only"}}, 403);
      std::lock_guard hardware_lock(hardware_mutex);
      stop_hardware();
      reply(res, {{"ok", true}});
    });
    server.Post("/api/heartbeat", [this](const auto& req, auto& res) {
      const auto body = Json::parse(req.body);
      auto device = validate_device(body, req.remote_addr);
      std::lock_guard lock(mutex);
      const auto id = device.at("id").template get<std::string>();
      const auto old = devices.find(id);
      if (old != devices.end() && old->second.at("instance") != device.at("instance") &&
          is_online(old->second))
        return reply(res, {{"error", "Device ID is already in use: " + id}}, 409);
      if (old == devices.end() && devices.size() >= 256)
        throw std::runtime_error("Device registry is full");
      if (old == devices.end() || !is_online(old->second))
        event(device.at("name").template get<std::string>() + " registered (" +
              device.at("role").template get<std::string>() + ")");
      devices[id] = device;
      reply(res, plan(id));
    });
    server.Post("/api/goodbye", [this](const auto& req, auto& res) {
      const auto body = Json::parse(req.body);
      const auto id = body.at("id").template get<std::string>();
      const auto instance = body.at("instance").template get<std::string>();
      std::lock_guard lock(mutex);
      const auto it = devices.find(id);
      if (it == devices.end() || it->second.at("instance") != instance ||
          it->second.at("host") != req.remote_addr)
        return reply(res, {{"error", "Device instance does not match"}}, 409);
      it->second["lastSeen"] = 0;
      event(it->second.at("name").template get<std::string>() + " disconnected");
      reply(res, {{"ok", true}});
    });
    server.Post("/api/routes", [this](const auto& req, auto& res) {
      const auto route = Json::parse(req.body);
      std::lock_guard lock(mutex);
      set_route_locked(route);
      reply(res, {{"ok", true}});
    });
    server.Delete("/api/routes", [this](const auto& req, auto& res) {
      const auto body = Json::parse(req.body);
      for (const auto* field : {"receiver", "input"})
        if (!body.contains(field) || !body[field].is_string() ||
            !valid_id(body[field].template get<std::string>()))
          throw std::runtime_error("Invalid receiver input");
      std::lock_guard lock(mutex);
      auto updated = routes;
      if (body.contains("receiverChannel")) {
        const auto side = bounded_int(body, "receiverChannel", 1, 2);
        split_stereo_route(updated, body.at("receiver"), body.at("input"));
        (void)side;
      } else {
        for (int side = 1; side <= 2; ++side)
          updated.erase(body.at("receiver").template get<std::string>() + ":" +
                        body.at("input").template get<std::string>() + ":" + std::to_string(side));
      }
      updated.erase(route_key(body));
      save(updated);
      routes = std::move(updated);
      ++revision;
      event("Disconnected " + route_key(body));
      reply(res, {{"ok", true}});
    });
    server.Get("/api/routes/export", [this](const auto&, auto& res) {
      std::lock_guard lock(mutex);
      res.set_header("Content-Disposition", "attachment; filename=routes.json");
      reply(res, {{"version", 1}, {"routes", route_array(routes)}});
    });
    server.Get(
        R"(/api/audio/([a-zA-Z0-9_-]+)/([a-zA-Z0-9_-]+)\.wav)", [this](const auto& req, auto& res) {
          Json device;
          const std::string input = req.matches[2];
          {
            std::lock_guard lock(mutex);
            const auto d = devices.find(req.matches[1]);
            if (d == devices.end() || !is_online(d->second) || d->second.at("inputs").empty() ||
                find_channel(d->second, "inputs", input).is_null())
              return reply(res, {{"error", "Receiver input is unavailable"}}, 404);
            device = d->second;
          }
          httplib::Client client(device.at("host").template get<std::string>(),
                                 device.at("monitorPort").template get<int>());
          client.set_connection_timeout(1, 0);
          client.set_read_timeout(2, 0);
          auto result = client.Get("/audio/" + input + ".wav");
          if (!result || result->status != 200)
            return reply(res, {{"error", "No captured audio; connect a stream first"}}, 409);
          res.set_header("Cache-Control", "no-store");
          res.set_header("Content-Disposition",
                         "inline; filename=" + device.at("id").template get<std::string>() + "-" +
                             input + ".wav");
          res.set_content(std::move(result->body), "audio/wav");
        });
    const std::map<std::string, std::pair<std::string, std::string>> assets = {
        {"/", {"index.html", "text/html; charset=utf-8"}},
        {"/app.js", {"app.js", "text/javascript; charset=utf-8"}},
        {"/style.css", {"style.css", "text/css; charset=utf-8"}}};
    for (const auto& [path, asset] : assets)
      server.Get(path, [this, asset](const auto&, auto& res) {
        std::ifstream file(std::filesystem::path(config.web_root) / asset.first, std::ios::binary);
        if (!file)
          return reply(
              res, {{"error", "Web assets missing. Use --web-dir to locate the web folder."}}, 404);
        const std::string content((std::istreambuf_iterator<char>(file)),
                                  std::istreambuf_iterator<char>());
        res.set_header("X-Content-Type-Options", "nosniff");
        res.set_header("Cache-Control", "no-cache");
        res.set_content(content, asset.second);
      });
    if (config.port == 0)
      port = server.bind_to_any_port(config.host);
    else {
      port = config.port;
      if (!server.bind_to_port(config.host, port))
        port = -1;
    }
    if (port <= 0)
      throw std::runtime_error("Cannot bind controller HTTP port");
  }
  void start() {
    thread = std::thread([this] { server.listen_after_bind(); });
  }
  ~Impl() {
    server.stop();
    if (thread.joinable())
      thread.join();
    stop_hardware();
  }
  void stop_hardware() {
    const bool had_sender = bool(local_sender), had_receiver = bool(local_receiver);
    local_sender.reset();
    local_receiver.reset();
    local_audio_active = false;
    std::lock_guard lock(mutex);
    local_audio_config = nullptr;
    if (had_sender)
      devices.erase("local-microphone");
    if (had_receiver)
      devices.erase("local-speakers");
    if (had_sender || had_receiver)
      event("Local sound card endpoints stopped");
  }
  void start_hardware(const Json& body) {
    std::lock_guard hardware_lock(hardware_mutex);
    if (local_audio_active)
      throw std::runtime_error("Stop local audio before changing its devices");
    const auto capture = body.value("capture", std::string{}),
               playback = body.value("playback", std::string{});
    if ((capture.empty() && playback.empty()) || capture.size() > 1024 || playback.size() > 1024)
      throw std::runtime_error("Select a microphone or playback device");
    const int streams = bounded_int(body, "streams", 1, 16),
              inputs = bounded_int(body, "inputs", 1, 16);
    const int volume = bounded_int(body, "volume", 0, 100),
              latency = bounded_int(body, "latency", 20, 500);
    auto map = [&](const char* key) {
      const auto list = body.value(key, Json::array());
      if (!list.is_array() || list.size() > 32)
        throw std::runtime_error("Invalid hardware channel map");
      std::vector<int> result;
      for (const auto& value : list) {
        if (!value.is_number_integer() || value.template get<std::int64_t>() < 1 ||
            value.template get<std::int64_t>() > 32)
          throw std::runtime_error("Physical channels must be 1-32");
        result.push_back(value.template get<int>());
      }
      return result;
    };
    const auto capture_map = map("captureMap"), playback_map = map("playbackMap");
    {
      std::lock_guard lock(mutex);
      for (const auto* id : {"local-microphone", "local-speakers"}) {
        const auto it = devices.find(id);
        if (it != devices.end() && is_online(it->second))
          throw std::runtime_error(std::string("Device ID already in use: ") + id);
      }
    }
    const auto url = "http://" + local_control_host(config.host) + ":" + std::to_string(port);
    try {
      if (!capture.empty()) {
        SenderConfig c;
        c.id = "local-microphone";
        c.name = "Local microphone";
        c.controller = url;
        c.host = config.host;
        c.capture_device = capture;
        c.capture_channels = capture_map;
        c.streams = streams;
        local_sender = std::make_unique<Sender>(c);
      }
      if (!playback.empty()) {
        ReceiverConfig c;
        c.id = "local-speakers";
        c.name = "Local sound card";
        c.controller = url;
        c.host = config.host;
        c.port = 0;
        c.playback_device = playback;
        c.playback_channels = playback_map;
        c.inputs = inputs;
        c.volume = volume / 100.0f;
        c.latency_ms = latency;
        local_receiver = std::make_unique<Receiver>(c);
      }
      local_audio_active = true;
      std::lock_guard lock(mutex);
      local_audio_config = {{"capture", capture},        {"playback", playback},
                            {"streams", streams},        {"inputs", inputs},
                            {"captureMap", capture_map}, {"playbackMap", playback_map},
                            {"volume", volume},          {"latency", latency}};
      event("Local sound card endpoints started; route audio in the matrix");
    } catch (...) {
      stop_hardware();
      throw;
    }
  }
  void event(const std::string& text) {
    events.insert(events.begin(), Json{{"at", epoch_ms()}, {"message", text}});
    if (events.size() > 60)
      events.erase(events.end() - 1);
  }
  static Json route_array(const std::map<std::string, Json>& values) {
    auto result = Json::array();
    for (const auto& [id, r] : values)
      result.push_back(r);
    return result;
  }
  void save(const std::map<std::string, Json>& updated) {
    if (!config.state_file.empty())
      write_atomic(
          config.state_file,
          {{"version", 1}, {"routes", route_array(updated)}, {"deviceNames", device_names},{"channelNames",channel_names}});
  }
  Json validate_device(const Json& body, const std::string& host) {
    if (!body.is_object() || !body.contains("id") || !body["id"].is_string() ||
        !valid_id(body["id"].get<std::string>()) || !valid_ipv4(host))
      throw std::runtime_error("Invalid device identity or IPv4 address");
    const auto role = body.at("role").get<std::string>(), name = body.at("name").get<std::string>(),
               instance = body.at("instance").get<std::string>();
    if ((role != "sender" && role != "receiver" && role != "device") || name.empty() ||
        name.size() > 80 || instance.empty() || instance.size() > 80)
      throw std::runtime_error("Invalid device name, role or instance");
    Json device = {{"id", body["id"]},         {"name", name},           {"role", role},
                   {"instance", instance},     {"host", host},           {"lastSeen", epoch_ms()},
                   {"streams", Json::array()}, {"inputs", Json::array()}};
    for (const auto* field : {"streams", "inputs"}) {
      const auto items = body.value(field, Json::array());
      if (!items.is_array() || items.size() > 16)
        throw std::runtime_error("Expected 0-16 endpoints per direction");
      std::vector<std::string> ids;
      for (const auto& item : items) {
        const auto id = item.at("id").get<std::string>(),
                   label = item.at("name").get<std::string>();
        if (!valid_id(id) || label.size() > 120 ||
            std::find(ids.begin(), ids.end(), id) != ids.end())
          throw std::runtime_error("Invalid or duplicate channel");
        ids.push_back(id);
        Json channel = {
            {"id", id}, {"name", label}, {"stats", item.value("stats", Json::object())}};
        channel["channels"] = item.contains("channels") ? bounded_int(item, "channels", 1, 2) : 2;
        channel["labels"]=Json::array();
        const auto labels=item.value("labels",Json::array());
        if(!labels.is_array()||labels.size()>2)throw std::runtime_error("Invalid channel labels");
        for(int side=0;side<channel["channels"].get<int>();++side){std::string text="Channel "+std::to_string((ids.size()-1)*2+side+1);if(side<int(labels.size()))text=labels[side].get<std::string>();if(text.size()>80)throw std::runtime_error("Channel label exceeds 80 characters");channel["labels"].push_back(text);}
        if (!channel["stats"].is_object())
          throw std::runtime_error("Invalid channel stats");
        if (std::string(field) == "streams") {
          if (!item.at("ssrc").is_number_unsigned() && !item.at("ssrc").is_number_integer())
            throw std::runtime_error("Invalid SSRC");
          const auto ssrc = item["ssrc"].get<std::int64_t>();
          if (ssrc < 0 || ssrc > 0xffffffffLL)
            throw std::runtime_error("Invalid SSRC");
          channel["ssrc"] = ssrc;
          channel["source"] = item.value("source", std::string{}).substr(0, 120);
        } else
          channel["port"] = bounded_int(item, "port", 1024, 65535);
        device[field].push_back(channel);
      }
    }
    if ((device["streams"].empty() && device["inputs"].empty()) ||
        (role == "sender" && (device["streams"].empty() || !device["inputs"].empty())) ||
        (role == "receiver" && (device["inputs"].empty() || !device["streams"].empty())))
      throw std::runtime_error("Device must advertise its enabled send/receive endpoints");
    if (!device["inputs"].empty())
      device["monitorPort"] = bounded_int(body, "monitorPort", 1024, 65535);
    const auto audio = body.value("audio", Json::object());
    if (!audio.is_object() || audio.dump().size() > 4096)
      throw std::runtime_error("Invalid hardware audio status");
    device["audio"] = audio;
    const auto format =
        body.value("format", Json{{"sampleRate", 48000}, {"bits", 16}, {"channels", 2}});
    if (!format.is_object() || !supported_sample_rate(format.at("sampleRate").get<int>()))
      throw std::runtime_error("Unsupported audio sample rate");
    device["format"] = format;
    device["clock"] = body.value(
        "clock", Json{{"mode", "software"}, {"synchronized", false}, {"ptpSupported", false}});
    const auto& clock=device["clock"];
    if(!clock.is_object()||clock.dump().size()>4096)throw std::runtime_error("Invalid clock status");
    for(const auto* key:{"source","mode","state","timestamping","interface","error"})if(clock.contains(key)&&!clock.at(key).is_string())throw std::runtime_error("Invalid clock status text");
    if(clock.contains("domain"))bounded_int(clock,"domain",0,255);
    for(const auto* key:{"source","mode","state","timestamping","interface","error","masterPortIdentity","grandmasterIdentity","masterAddress"})
      if(clock.contains(key)&&!clock.at(key).is_null()&&(!clock.at(key).is_string()||clock.at(key).get<std::string>().size()>256))throw std::runtime_error("Invalid clock status text");
    for(const auto* key:{"synchronized","locked","audioLocked","ptpSupported","ptpAvailable","experimental","trackingStable","measurementFresh"})
      if(clock.contains(key)&&!clock.at(key).is_boolean())throw std::runtime_error("Invalid clock status flag");
    for(const auto* key:{"offsetNs","pathDelayNs","frequencyPpm","domain"})if(clock.contains(key)&&!clock.at(key).is_null()&&!clock.at(key).is_number())throw std::runtime_error("Invalid clock measurement");
    return device;
  }
  Json named(Json d)const{
    const auto id=d.at("id").get<std::string>();if(device_names.contains(id))d["name"]=device_names.at(id);
    for(const auto* direction:{"streams","inputs"})for(auto& endpoint:d[direction])for(int side=1;side<=endpoint.value("channels",2);++side){const auto key=id+":"+direction+":"+endpoint.at("id").get<std::string>()+":"+std::to_string(side);if(channel_names.contains(key))endpoint["labels"][side-1]=channel_names.at(key);}
    return d;
  }
  bool available(const Json& route) const {
    const auto s = devices.find(route.at("sender").get<std::string>()),
               r = devices.find(route.at("receiver").get<std::string>());
    return s != devices.end() && r != devices.end() && is_online(s->second) &&
           is_online(r->second) &&
           s->second.at("format").at("sampleRate") == r->second.at("format").at("sampleRate") &&
           route_channels_available(route, find_channel(s->second, "streams", route.at("stream")),
                                    find_channel(r->second, "inputs", route.at("input")));
  }
  Json plan(const std::string& id) const {
    Json result = {{"revision", revision}, {"sends", Json::array()}, {"receives", Json::array()}};
    for (const auto& [key, route] : routes) {
      if (!available(route))
        continue;
      const auto& s = devices.at(route.at("sender").get<std::string>());
      const auto& r = devices.at(route.at("receiver").get<std::string>());
      const auto stream = find_channel(s, "streams", route.at("stream")),
                 input = find_channel(r, "inputs", route.at("input"));
      if (route.at("sender") == id)
        result["sends"].push_back({{"stream", route.at("stream")},
                                   {"host", r.at("host")},
                                   {"port", input.at("port")},
                                   {"receiver", route.at("receiver")},
                                   {"input", route.at("input")}});
      if (route.at("receiver") == id)
        result["receives"].push_back({{"input", route.at("input")},
                                      {"sender", route.at("sender")},
                                      {"stream", route.at("stream")},
                                      {"ssrc", stream.at("ssrc")},
                                      {"label", s.at("name").get<std::string>() + " / " +
                                                    stream.at("name").get<std::string>()}});
      if (route.contains("receiverChannel") && route.at("receiver") == id) {
        result["receives"].back()["receiverChannel"] = route.at("receiverChannel");
        result["receives"].back()["senderChannel"] = route.at("senderChannel");
      }
    }
    return result;
  }
  void set_route_locked(const Json& body) {
    validate_route(body);
    const auto s = devices.find(body.at("sender")), r = devices.find(body.at("receiver"));
    if (s == devices.end() || r == devices.end() ||
        find_channel(s->second, "streams", body.at("stream")).is_null() ||
        find_channel(r->second, "inputs", body.at("input")).is_null())
      throw std::runtime_error("Unknown source or receiver input");
    Json route;
    for (const auto* field : {"sender", "stream", "receiver", "input"})
      route[field] = body.at(field);
    if (s->second.at("format").at("sampleRate") != r->second.at("format").at("sampleRate"))
      throw std::runtime_error("Source and destination sample rates do not match");
    if (!route_channels_available(body, find_channel(s->second, "streams", body.at("stream")),
                                  find_channel(r->second, "inputs", body.at("input"))))
      throw std::runtime_error("The selected physical channel is not advertised by this device");
    for (const auto* field : {"receiverChannel", "senderChannel"})
      if (body.contains(field))
        route[field] = body.at(field);
    if (!route.contains("receiverChannel") &&
        find_channel(r->second, "inputs", body.at("input")).value("channels", 2) == 1) {
      route["receiverChannel"] = 1;
      route["senderChannel"] = 1;
    }
    auto updated = routes;
    if (route.contains("receiverChannel"))
      split_stereo_route(updated, route.at("receiver"), route.at("input"));
    else
      for (int side = 1; side <= 2; ++side)
        updated.erase(route.at("receiver").get<std::string>() + ":" +
                      route.at("input").get<std::string>() + ":" + std::to_string(side));
    updated[route_key(route)] = route;
    save(updated);
    routes = std::move(updated);
    ++revision;
    event("Connected " + s->second.at("name").get<std::string>() + " / " +
          body.at("stream").get<std::string>() + " -> " + r->second.at("name").get<std::string>() +
          " / " + body.at("input").get<std::string>());
  }
  Json state_locked() const {
    Json result = {
        {"name", "OpenAoIP"},
        {"version", "0.6.1"},
        {"revision", revision},
        {"localAudioActive", local_audio_active.load()},
        {"localAudioConfig", local_audio_config},
        {"serverTime", epoch_ms()},
        {"format", {{"sampleRate", sample_rate}, {"channels", 2}, {"bits", 16}, {"packetMs", 5}}},
        {"devices", Json::array()},
        {"routes", Json::array()},
        {"events", events}};
    for (const auto& [id, device] : devices) {
      auto value = named(device);
      value["online"] = is_online(device);
      result["devices"].push_back(value);
    }
    for (const auto& [id, route] : routes) {
      auto value = route;
      value["active"] = available(route);
      result["routes"].push_back(value);
    }
    return result;
  }
};
Controller::Controller(ControllerConfig config) : impl_(std::make_unique<Impl>(std::move(config))) {
  impl_->start();
}
Controller::~Controller() = default;
int Controller::port() const {
  return impl_->port;
}
bool Controller::fresh() const {
  return impl_->fresh;
}
Json Controller::state() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->state_locked();
}
void Controller::set_route(const Json& route) {
  std::lock_guard lock(impl_->mutex);
  impl_->set_route_locked(route);
}
void Controller::remove_route(const std::string& receiver, const std::string& input, int channel) {
  if (!valid_id(receiver) || !valid_id(input) || channel < 0 || channel > 2)
    throw std::runtime_error("Invalid destination channel");
  std::lock_guard lock(impl_->mutex);
  auto updated = impl_->routes;
  const auto base = receiver + ":" + input;
  if (channel) {
    split_stereo_route(updated, receiver, input);
    updated.erase(base + ":" + std::to_string(channel));
  } else {
    updated.erase(base);
    updated.erase(base + ":1");
    updated.erase(base + ":2");
  }
  impl_->save(updated);
  impl_->routes = std::move(updated);
  ++impl_->revision;
  impl_->event("Disconnected " + base);
}
void Controller::set_device_name(const std::string& id, const std::string& name) {
  if (!valid_id(id) || name.empty() || name.size() > 80)
    throw std::runtime_error("Device name must contain 1-80 characters");
  std::lock_guard lock(impl_->mutex);
  const auto previous = impl_->device_names;
  impl_->device_names[id] = name;
  try {
    impl_->save(impl_->routes);
  } catch (...) {
    impl_->device_names = previous;
    throw;
  }
  impl_->event("Device renamed: " + id + " -> " + name);
}
void Controller::set_channel_name(const std::string& id,const std::string& direction,const std::string& endpoint,int side,const std::string& name){
  if(!valid_id(id)||!valid_id(endpoint)||(direction!="streams"&&direction!="inputs")||side<1||side>2||name.empty()||name.size()>80)throw std::runtime_error("Invalid channel name or address");
  std::lock_guard lock(impl_->mutex);const auto d=impl_->devices.find(id);if(d==impl_->devices.end())throw std::runtime_error("Unknown device");const auto channel=find_channel(d->second,direction.c_str(),endpoint);if(channel.is_null()||side>channel.value("channels",2))throw std::runtime_error("Unknown physical channel");
  const auto old=impl_->channel_names;impl_->channel_names[id+":"+direction+":"+endpoint+":"+std::to_string(side)]=name;try{impl_->save(impl_->routes);}catch(...){impl_->channel_names=old;throw;}impl_->event("Channel renamed: "+id+" / "+name);
}
Json Controller::preset()const{std::lock_guard lock(impl_->mutex);return {{"version",1},{"routes",Impl::route_array(impl_->routes)},{"deviceNames",impl_->device_names},{"channelNames",impl_->channel_names}};}
void Controller::restore_preset(const Json& preset){
  if(!preset.is_object()||preset.value("version",0)!=1||!preset.contains("routes")||!preset.at("routes").is_array()||preset.at("routes").size()>8192)throw std::runtime_error("Invalid routing preset");
  std::map<std::string,Json> updated;std::set<std::string> destinations;
  for(const auto& raw:preset.at("routes")){validate_route(raw);Json r;for(const auto* f:{"sender","stream","receiver","input","senderChannel","receiverChannel"})if(raw.contains(f))r[f]=raw.at(f);
    for(int side=1;side<=2;++side)if(!r.contains("receiverChannel")||r.at("receiverChannel")==side){const auto key=r.at("receiver").get<std::string>()+":"+r.at("input").get<std::string>()+":"+std::to_string(side);if(!destinations.insert(key).second)throw std::runtime_error("Preset assigns a receiver channel more than once");}updated[route_key(r)]=r;
  }
  const auto names=preset.value("deviceNames",Json::object()),channels=preset.value("channelNames",Json::object());
  for(const auto& values:{names,channels}){if(!values.is_object()||values.size()>16384)throw std::runtime_error("Invalid preset aliases");for(const auto& [key,value]:values.items())if(key.size()>220||!value.is_string()||value.get<std::string>().size()>80)throw std::runtime_error("Invalid preset name");}
  std::lock_guard lock(impl_->mutex);const auto old_names=impl_->device_names,old_channels=impl_->channel_names;impl_->device_names=names;impl_->channel_names=channels;
  try{impl_->save(updated);}catch(...){impl_->device_names=old_names;impl_->channel_names=old_channels;throw;}impl_->routes=std::move(updated);++impl_->revision;impl_->event("Routing preset restored ("+std::to_string(impl_->routes.size())+" routes)");
}
} // namespace aoip
