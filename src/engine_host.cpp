#include "aoip/engine_host.hpp"
#ifdef _WIN32
#include "aoip/bridge_engine.hpp"
#include "aoip/network.hpp"
#include "aoip/version.hpp"
#include "aoip/vsc_engine.hpp"
#include "httplib.h"
#include <bcrypt.h>
#include <mutex>
#include <thread>
#include <windows.h>
namespace aoip::engine {
namespace {
std::string secure_token() {
  std::array<unsigned char, 32> bytes{};
  if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                      BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
    throw std::runtime_error("Cannot initialize local engine authentication");
  constexpr char hex[] = "0123456789abcdef";
  std::string out;
  for (auto b : bytes) {
    out += hex[b >> 4];
    out += hex[b & 15];
  }
  return out;
}
struct Record {
  std::string kind;
  Json config;
  std::unique_ptr<vsc::Engine> vsc;
  std::unique_ptr<bridge::Engine> bridge;
  Json status() const {
    auto s = vsc ? vsc->status() : bridge->status();
    s["active"] = true;
    s["config"] = config;
    return s;
  }
};
Json devices(AudioBackend backend) {
  const auto list = audio_devices(backend);
  Json result = {
      {"backend", list.backend}, {"capture", Json::array()}, {"playback", Json::array()}};
  for (const auto& d : list.capture)
    result["capture"].push_back(
        {{"id", d.id}, {"name", d.name}, {"default", d.is_default}, {"index", d.index}});
  for (const auto& d : list.playback)
    result["playback"].push_back(
        {{"id", d.id}, {"name", d.name}, {"default", d.is_default}, {"index", d.index}});
  return result;
}
} // namespace
struct Host::Impl {
  Options options;
  httplib::Server server;
  std::thread thread;
  std::atomic<bool> running{true};
  std::atomic<double> touched{0};
  mutable std::mutex mutex;
  std::map<std::string, std::unique_ptr<Record>> records;
  HANDLE singleton = nullptr;
  int port = 0;
  std::string token = secure_token();
  explicit Impl(Options o) : options(std::move(o)) {
    const auto lock_name = L"Local\\OpenAoIP.Engine." +
                           std::to_wstring(std::hash<std::wstring>{}(
                               std::filesystem::absolute(options.endpoint_file).wstring()));
    singleton = CreateMutexW(nullptr, TRUE, lock_name.c_str());
    if (!singleton)
      throw std::runtime_error("Cannot create engine instance lock");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      CloseHandle(singleton);
      singleton = nullptr;
      throw std::runtime_error("An engine already owns this user-session endpoint");
    }
    server.set_payload_max_length(2 * 1024 * 1024);
    server.set_read_timeout(2, 0);
    server.set_write_timeout(2, 0);
    server.set_pre_routing_handler([this](const auto& req, auto& res) {
      touched = monotonic_ms();
      if (req.has_header("Origin") || req.get_header_value("X-OpenAoIP-Token") != token) {
        res.status = 403;
        res.set_content("{\"error\":\"Unauthorized local engine client\"}", "application/json");
        return httplib::Server::HandlerResponse::Handled;
      }
      return httplib::Server::HandlerResponse::Unhandled;
    });
    server.set_exception_handler([](const auto&, auto& res, std::exception_ptr error) {
      try {
        std::rethrow_exception(error);
      } catch (const std::exception& e) {
        res.status = 400;
        res.set_content(Json{{"error", e.what()}}.dump(), "application/json");
      }
    });
    server.Get("/v1/status", [](const auto&, auto& res) {
      res.set_content(Json{{"protocol", engine_protocol},
                           {"version", version},
                           {"pid", GetCurrentProcessId()},
                           {"mode", "per-user session engine"}}
                          .dump(),
                      "application/json");
    });
    server.Get("/v1/devices", [this](const auto&, auto& res) {
      res.set_content(devices(options.backend).dump(), "application/json");
    });
    server.Get(R"(/v1/engines/([a-zA-Z0-9_-]+))", [this](const auto& req, auto& res) {
      res.set_content(status(req.matches[1]).dump(), "application/json");
    });
    server.Put(R"(/v1/engines/([a-zA-Z0-9_-]+))", [this](const auto& req, auto& res) {
      const auto value = Json::parse(req.body);
      if (value.at("config").at("id") != req.matches[1].str())
        throw std::runtime_error("Session identity mismatch");
      res.set_content(start(value.at("kind"), value.at("config")).dump(), "application/json");
    });
    server.Delete(R"(/v1/engines/([a-zA-Z0-9_-]+))", [this](const auto& req, auto& res) {
      stop_session(req.matches[1]);
      res.set_content("{\"active\":false,\"running\":false}", "application/json");
    });
    server.Delete("/v1/idle", [this](const auto&, auto& res) {
      std::lock_guard lock(mutex);
      const bool idle = records.empty();
      if (idle)
        running = false;
      res.set_content(Json{{"exiting", idle}}.dump(), "application/json");
    });
    port = server.bind_to_any_port("127.0.0.1");
    if (port <= 0) {
      CloseHandle(singleton);
      singleton = nullptr;
      throw std::runtime_error("Cannot bind the local engine API");
    }
    touched = monotonic_ms();
    try {
      thread = std::thread([this] {
        server.listen_after_bind();
        running = false;
      });
      write_configuration(options.endpoint_file, {{"protocol", engine_protocol},
                                                  {"pid", GetCurrentProcessId()},
                                                  {"port", port},
                                                  {"token", token}});
    } catch (...) {
      server.stop();
      if (thread.joinable())
        thread.join();
      CloseHandle(singleton);
      singleton = nullptr;
      throw;
    }
  }
  ~Impl() {
    running = false;
    server.stop();
    if (thread.joinable())
      thread.join();
    records.clear();
    try {
      if (std::filesystem::exists(options.endpoint_file) &&
          read_configuration(options.endpoint_file).value("token", "") == token)
        std::filesystem::remove(options.endpoint_file);
    } catch (...) {
    }
    if (singleton) {
      ReleaseMutex(singleton);
      CloseHandle(singleton);
    }
  }
  Json start(const std::string& kind, const Json& config) {
    const auto id = config.at("id").get<std::string>();
    if (!valid_id(id))
      throw std::runtime_error("Invalid session ID");
    std::lock_guard lock(mutex);
    if (records.contains(id))
      throw std::runtime_error(
          "Session is already running; stop it before changing audio settings");
    auto r = std::make_unique<Record>();
    r->kind = kind;
    r->config = config;
    if (kind == "bridge")
      r->bridge = std::make_unique<bridge::Engine>(bridge::from_json(config), options.backend);
    else if (kind == "vsc")
      r->vsc = std::make_unique<vsc::Engine>(vsc::from_json(config));
    else
      throw std::runtime_error("Unknown audio-engine mode");
    auto status = r->status();
    records.emplace(id, std::move(r));
    return status;
  }
  void stop_session(const std::string& id) {
    std::unique_ptr<Record> retired;
    {
      std::lock_guard lock(mutex);
      auto it = records.find(id);
      if (it != records.end()) {
        retired = std::move(it->second);
        records.erase(it);
      }
    }
    retired.reset();
  }
  Json status(const std::string& id) const {
    std::lock_guard lock(mutex);
    const auto it = records.find(id);
    return it == records.end() ? Json{{"active", false}, {"running", false}} : it->second->status();
  }
};
Host::Host(Options o) : impl_(std::make_unique<Impl>(std::move(o))) {}
Host::~Host() = default;
void Host::run() {
  while (impl_->running) {
    if (impl_->options.idle_seconds > 0 &&
        monotonic_ms() - impl_->touched > impl_->options.idle_seconds * 1000.) {
      std::lock_guard lock(impl_->mutex);
      if (impl_->records.empty()) {
        stop();
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}
void Host::stop() {
  impl_->running = false;
  impl_->server.stop();
}
int Host::port() const {
  return impl_->port;
}
Json Host::start(const std::string& kind, const Json& c) {
  return impl_->start(kind, c);
}
void Host::stop_session(const std::string& id) {
  impl_->stop_session(id);
}
Json Host::status(const std::string& id) const {
  return impl_->status(id);
}
Client::Client(std::filesystem::path exe, std::filesystem::path endpoint)
    : executable_(std::move(exe)), endpoint_(std::move(endpoint)) {}
void Client::connect() {
  auto probe = [&] {
    try {
      const auto value = read_configuration(endpoint_, 8192);
      if (value.at("protocol") != engine_protocol)
        return false;
      url_ = "http://127.0.0.1:" + std::to_string(value.at("port").get<int>());
      token_ = value.at("token");
      httplib::Client client(url_);
      client.set_connection_timeout(0, 200000);
      client.set_read_timeout(0, 300000);
      const auto res = client.Get("/v1/status", httplib::Headers{{"X-OpenAoIP-Token", token_}});
      return res && res->status == 200 &&
             Json::parse(res->body).value("protocol", 0) == engine_protocol;
    } catch (...) {
      return false;
    }
  };
  if (probe())
    return;
  if (!std::filesystem::exists(executable_))
    throw std::runtime_error("OpenAoIP Engine.exe is missing beside this application");
  const auto command =
      L"\"" + executable_.wstring() + L"\" --endpoint-file \"" + endpoint_.wstring() + L"\"";
  std::vector<wchar_t> writable(command.begin(), command.end());
  writable.push_back(0);
  STARTUPINFOW startup{sizeof(startup)};
  PROCESS_INFORMATION child{};
  if (!CreateProcessW(executable_.c_str(), writable.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, executable_.parent_path().c_str(), &startup,
                      &child))
    throw std::runtime_error("Cannot start the per-user audio engine");
  CloseHandle(child.hThread);
  CloseHandle(child.hProcess);
  for (int i = 0; i < 50; ++i) {
    if (probe())
      return;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  throw std::runtime_error("Audio engine startup timed out");
}
Json Client::request(const std::string& method, const std::string& path, const Json& body) {
  if (url_.empty())
    connect();
  httplib::Client client(url_);
  client.set_connection_timeout(0, 300000);
  client.set_read_timeout(3, 0);
  client.set_write_timeout(2, 0);
  httplib::Headers headers = {{"X-OpenAoIP-Token", token_}};
  const auto result = method == "GET"   ? client.Get(path, headers)
                      : method == "PUT" ? client.Put(path, headers, body.dump(), "application/json")
                                        : client.Delete(path, headers);
  if (!result) {
    url_.clear();
    throw std::runtime_error("Audio engine is not reachable");
  }
  const auto value = Json::parse(result->body);
  if (result->status != 200)
    throw std::runtime_error(value.value("error", "Audio engine request failed"));
  return value;
}
Json Client::status(const std::string& id) {
  if (!valid_id(id))
    throw std::runtime_error("Invalid session ID");
  return request("GET", "/v1/engines/" + id);
}
Json Client::start(const std::string& kind, const Json& config) {
  return request("PUT", "/v1/engines/" + config.at("id").get<std::string>(),
                 {{"kind", kind}, {"config", config}});
}
void Client::stop(const std::string& id) {
  if (!valid_id(id))
    throw std::runtime_error("Invalid session ID");
  request("DELETE", "/v1/engines/" + id);
}
Json Client::devices() {
  return request("GET", "/v1/devices");
}
void Client::release_if_idle() {
  if (!url_.empty())
    request("DELETE", "/v1/idle");
}
} // namespace aoip::engine
#endif
