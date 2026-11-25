#pragma once
#ifdef _WIN32
#include "aoip/config_store.hpp"
namespace aoip::engine {
struct Options {
  std::filesystem::path endpoint_file = engine_endpoint_file();
  int idle_seconds = 60;
  AudioBackend backend = AudioBackend::Wasapi;
};
class Host {
public:
  explicit Host(Options = {});
  ~Host();
  void run();
  void stop();
  int port() const;
  Json start(const std::string& kind, const Json& config);
  void stop_session(const std::string& id);
  Json status(const std::string& id) const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
class Client {
public:
  explicit Client(std::filesystem::path engine_executable,
                  std::filesystem::path endpoint_file = engine_endpoint_file());
  Json status(const std::string& id);
  Json start(const std::string& kind, const Json& config);
  void stop(const std::string& id);
  void release_if_idle();
  Json devices();

private:
  std::filesystem::path executable_, endpoint_;
  std::string url_, token_;
  void connect();
  Json request(const std::string& method, const std::string& path,
               const Json& body = Json::object());
};
} // namespace aoip::engine
#endif
