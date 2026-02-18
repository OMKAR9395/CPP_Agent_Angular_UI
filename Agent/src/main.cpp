#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

#include "api/HttpServer.h"
#include "api/WebSocketServer.h"
#include "core/EventBus.h"
#include "core/SessionManager.h"
#include "serial/PortEnumerator.h"
#include "serial/SerialManager.h"

namespace {

agent::HttpServer* g_http_server = nullptr;

std::string EnvOrDefault(const char* name, const std::string& default_value) {
  const char* value = std::getenv(name);
  if (value == nullptr || std::string(value).empty()) {
    return default_value;
  }
  return value;
}

int EnvIntOrDefault(const char* name, int default_value) {
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return default_value;
  }
  try {
    return std::stoi(value);
  } catch (...) {
    return default_value;
  }
}

bool EnvBoolOrDefault(const char* name, bool default_value) {
  const char* value = std::getenv(name);
  if (value == nullptr) {
    return default_value;
  }
  const std::string v(value);
  return v == "1" || v == "true" || v == "TRUE" || v == "yes" || v == "YES";
}

void HandleSignal(int) {
  if (g_http_server != nullptr) {
    g_http_server->Stop();
  }
}

}  // namespace

int main() {
  const std::string host = "127.0.0.1";
  const int http_port = EnvIntOrDefault("AGENT_PORT", 8080);
  const int ws_port = EnvIntOrDefault("AGENT_WS_PORT", http_port + 1);
  const std::string cors_origins =
      EnvOrDefault("CORS_ORIGINS", "http://localhost:3000,http://127.0.0.1:3000");
  const bool mock_serial = EnvBoolOrDefault("AGENT_MOCK_SERIAL", false);
  const std::string version = "0.1.0";

  agent::EventBus event_bus;
  agent::PortEnumerator port_enumerator;
  agent::SerialManager serial_manager(event_bus, port_enumerator);
  serial_manager.EnableMockSerial(mock_serial);
  serial_manager.Start();

  agent::WebSocketServer ws_server(event_bus, host, static_cast<std::uint16_t>(ws_port));
  std::string ws_error;
  if (!ws_server.Start(&ws_error)) {
    std::cerr << "Failed to start WebSocket server: " << ws_error << std::endl;
    serial_manager.Stop();
    return 1;
  }

  agent::SessionManager session_manager;
  agent::HttpServer http_server(serial_manager, session_manager, event_bus, host, http_port, ws_port,
                                version, cors_origins);

  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);
  g_http_server = &http_server;

  std::cout << "Device Agent started\n";
  std::cout << "HTTP: http://" << host << ":" << http_port << "\n";
  std::cout << "WS:   ws://" << host << ":" << ws_port << "/ws\n";
  std::cout << "Mock serial: " << (mock_serial ? "enabled" : "disabled") << std::endl;

  std::string http_error;
  const bool http_ok = http_server.Start(&http_error);
  g_http_server = nullptr;

  ws_server.Stop();
  serial_manager.Stop();

  if (!http_ok) {
    std::cerr << "HTTP server failed: " << http_error << std::endl;
    return 1;
  }
  return 0;
}
