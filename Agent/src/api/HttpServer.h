#pragma once

#include <chrono>
#include <string>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "core/EventBus.h"
#include "core/SessionManager.h"
#include "serial/SerialManager.h"

namespace agent {

class HttpServer {
 public:
  HttpServer(SerialManager& serial_manager, SessionManager& session_manager, EventBus& event_bus,
             std::string host, int port, int ws_port, std::string version,
             std::string cors_origins_csv);

  bool Start(std::string* error_message = nullptr);
  void Stop();

 private:
  void RegisterRoutes();
  bool ParseJsonBody(const httplib::Request& req, nlohmann::json* out, httplib::Response& res) const;
  void ApplyCors(const httplib::Request& req, httplib::Response& res) const;
  bool IsOriginAllowed(const std::string& origin) const;
  static std::vector<std::string> ParseCsv(const std::string& csv);

  SerialManager& serial_manager_;
  SessionManager& session_manager_;
  EventBus& event_bus_;
  std::string host_;
  int port_ = 0;
  int ws_port_ = 0;
  std::string version_;
  std::vector<std::string> allowed_origins_;
  std::chrono::steady_clock::time_point started_at_;

  httplib::Server server_;
};

}  // namespace agent
