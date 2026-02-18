#include "api/HttpServer.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>

namespace agent {
namespace {

void WriteJson(httplib::Response& res, const nlohmann::json& body, int status = 200) {
  res.status = status;
  res.set_content(body.dump(), "application/json");
}

std::string Trim(std::string value) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
  value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
  return value;
}

int ClampInt(int value, int min_value, int max_value) {
  return std::max(min_value, std::min(max_value, value));
}

}  // namespace

HttpServer::HttpServer(SerialManager& serial_manager, SessionManager& session_manager, EventBus& event_bus,
                       std::string host, int port, int ws_port, std::string version,
                       std::string cors_origins_csv)
    : serial_manager_(serial_manager),
      session_manager_(session_manager),
      event_bus_(event_bus),
      host_(std::move(host)),
      port_(port),
      ws_port_(ws_port),
      version_(std::move(version)),
      allowed_origins_(ParseCsv(cors_origins_csv)),
      started_at_(std::chrono::steady_clock::now()) {}

bool HttpServer::Start(std::string* error_message) {
  RegisterRoutes();
  if (!server_.listen(host_.c_str(), port_)) {
    if (error_message != nullptr) {
      *error_message = "Failed to bind HTTP server to " + host_ + ":" + std::to_string(port_);
    }
    return false;
  }
  return true;
}

void HttpServer::Stop() {
  server_.stop();
}

void HttpServer::RegisterRoutes() {
  server_.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
    ApplyCors(req, res);
    if (req.method == "OPTIONS") {
      res.status = 204;
      return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });

  server_.set_post_routing_handler(
      [this](const httplib::Request& req, httplib::Response& res) { ApplyCors(req, res); });

  server_.Get("/api/v1/serial/devices", [this](const httplib::Request&, httplib::Response& res) {
    auto devices = serial_manager_.ListDevices();
    nlohmann::json list = nlohmann::json::array();
    for (const auto& d : devices) {
      list.push_back({{"port", d.port},
                      {"display_name", d.display_name},
                      {"unique_id", d.unique_id},
                      {"is_busy", d.is_busy},
                      {"notes", d.notes}});
    }
    WriteJson(res, list);
  });

  server_.Post("/api/v1/serial/validate", [this](const httplib::Request& req, httplib::Response& res) {
    nlohmann::json body;
    if (!ParseJsonBody(req, &body, res)) {
      return;
    }

    ValidateRequest validate;
    if (!body.contains("port") || !body["port"].is_string()) {
      WriteJson(res, {{"ok", false}, {"error", "INVALID_REQUEST"}, {"message", "port is required."}}, 400);
      return;
    }
    validate.port = body["port"].get<std::string>();
    validate.baud = body.value("baud", 9600);

    if (!body.contains("validate") || !body["validate"].is_object()) {
      WriteJson(
          res,
          {{"ok", false}, {"error", "INVALID_REQUEST"}, {"message", "validate object is required."}}, 400);
      return;
    }
    const auto& v = body["validate"];
    if (v.value("type", "") != "handshake") {
      WriteJson(res, {{"ok", false},
                      {"error", "INVALID_REQUEST"},
                      {"message", "validate.type must be 'handshake'."}},
                400);
      return;
    }
    if (!v.contains("command_b64") || !v["command_b64"].is_string()) {
      WriteJson(res,
                {{"ok", false}, {"error", "INVALID_REQUEST"}, {"message", "command_b64 is required."}}, 400);
      return;
    }
    if (!SerialManager::DecodeBase64(v["command_b64"].get<std::string>(), &validate.command_bytes)) {
      WriteJson(res, {{"ok", false}, {"error", "INVALID_BASE64"}, {"message", "command_b64 is invalid."}},
                400);
      return;
    }

    validate.expected_regex = v.value("expected_regex", "");
    validate.read_timeout_ms = ClampInt(v.value("read_timeout_ms", 500), 1, 30000);
    validate.max_read_bytes =
        static_cast<std::size_t>(ClampInt(v.value("max_read_bytes", 1024), 1, 65536));

    ValidateResult result = serial_manager_.Validate(validate);
    if (!result.ok) {
      WriteJson(res,
                {{"ok", false},
                 {"error", result.error_code},
                 {"message", result.error_message},
                 {"details", result.details}},
                result.http_status);
      return;
    }

    WriteJson(res, {{"ok", true},
                    {"valid", result.valid},
                    {"response_text", result.response_text},
                    {"response_b64", result.response_b64},
                    {"details", result.details}});
  });

  server_.Post("/api/v1/serial/connect", [this](const httplib::Request& req, httplib::Response& res) {
    nlohmann::json body;
    if (!ParseJsonBody(req, &body, res)) {
      return;
    }

    if (!body.contains("port") || !body["port"].is_string()) {
      WriteJson(res, {{"ok", false}, {"error", "INVALID_REQUEST"}, {"message", "port is required."}}, 400);
      return;
    }

    ConnectRequest connect;
    connect.port = body["port"].get<std::string>();
    connect.baud = body.value("baud", 9600);
    connect.unique_id = body.value("unique_id", "");

    if (body.contains("mode") && body["mode"].is_object()) {
      const auto& mode = body["mode"];
      connect.mode_type = mode.value("type", "line");
      connect.delimiter = mode.value("delimiter", "\n");
    }

    const SessionCredentials creds = session_manager_.CreateSession();
    ConnectResult result = serial_manager_.Connect(creds.session_id, connect);
    if (!result.ok) {
      session_manager_.RemoveSession(creds.session_id);
      WriteJson(res, {{"ok", false}, {"error", result.error_code}, {"message", result.error_message}},
                result.http_status);
      return;
    }

    WriteJson(res, {{"ok", true},
                    {"session_id", result.session_id},
                    {"owner_token", creds.owner_token},
                    {"port", result.port},
                    {"unique_id", result.unique_id}});
  });

  server_.Post("/api/v1/serial/disconnect", [this](const httplib::Request& req, httplib::Response& res) {
    nlohmann::json body;
    if (!ParseJsonBody(req, &body, res)) {
      return;
    }

    const std::string session_id = body.value("session_id", "");
    if (session_id.empty()) {
      WriteJson(res,
                {{"ok", false}, {"error", "INVALID_REQUEST"}, {"message", "session_id is required."}}, 400);
      return;
    }

    std::string error_code;
    std::string error_message;
    if (!serial_manager_.Disconnect(session_id, &error_code, &error_message)) {
      WriteJson(res, {{"ok", false}, {"error", error_code}, {"message", error_message}}, 404);
      return;
    }
    session_manager_.RemoveSession(session_id);
    WriteJson(res, {{"ok", true}});
  });

  server_.Post("/api/v1/serial/write", [this](const httplib::Request& req, httplib::Response& res) {
    nlohmann::json body;
    if (!ParseJsonBody(req, &body, res)) {
      return;
    }

    const std::string session_id = body.value("session_id", "");
    const std::string token = req.get_header_value("X-Owner-Token");
    if (session_id.empty()) {
      WriteJson(res,
                {{"ok", false}, {"error", "INVALID_REQUEST"}, {"message", "session_id is required."}}, 400);
      return;
    }
    if (!session_manager_.ValidateOwner(session_id, token)) {
      WriteJson(res,
                {{"ok", false}, {"error", "NOT_OWNER"}, {"message", "Invalid owner token for session."}},
                403);
      return;
    }

    std::vector<uint8_t> payload;
    if (!body.contains("data_b64") || !body["data_b64"].is_string() ||
        !SerialManager::DecodeBase64(body["data_b64"].get<std::string>(), &payload)) {
      WriteJson(res, {{"ok", false}, {"error", "INVALID_BASE64"}, {"message", "data_b64 is invalid."}},
                400);
      return;
    }

    WriteResult result = serial_manager_.EnqueueWrite(session_id, payload);
    if (!result.ok) {
      WriteJson(res, {{"ok", false}, {"error", result.error_code}, {"message", result.error_message}},
                result.http_status);
      return;
    }
    WriteJson(res, {{"ok", true}, {"bytes_written", result.bytes_written}});
  });

  server_.Get("/api/v1/serial/read", [this](const httplib::Request& req, httplib::Response& res) {
    const std::string session_id = req.has_param("session_id") ? req.get_param_value("session_id") : "";
    const std::string token = req.get_header_value("X-Owner-Token");
    if (session_id.empty()) {
      WriteJson(res,
                {{"ok", false}, {"error", "INVALID_REQUEST"}, {"message", "session_id is required."}}, 400);
      return;
    }
    if (!session_manager_.ValidateOwner(session_id, token)) {
      WriteJson(res,
                {{"ok", false}, {"error", "NOT_OWNER"}, {"message", "Invalid owner token for session."}},
                403);
      return;
    }

    int timeout_ms = 100;
    if (req.has_param("timeout_ms")) {
      timeout_ms = ClampInt(std::atoi(req.get_param_value("timeout_ms").c_str()), 0, 30000);
    }

    std::size_t max_bytes = 512;
    if (req.has_param("max")) {
      max_bytes =
          static_cast<std::size_t>(ClampInt(std::atoi(req.get_param_value("max").c_str()), 1, 65536));
    }

    ReadResult result = serial_manager_.ReadBuffered(session_id, timeout_ms, max_bytes);
    if (!result.ok) {
      WriteJson(res, {{"ok", false}, {"error", result.error_code}, {"message", result.error_message}},
                result.http_status);
      return;
    }

    WriteJson(res, {{"ok", true},
                    {"data_b64", result.data_b64},
                    {"data_text", result.data_text},
                    {"bytes", result.bytes}});
  });

  server_.Get("/api/v1/status", [this](const httplib::Request&, httplib::Response& res) {
    const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - started_at_)
                            .count();
    const auto statuses = serial_manager_.GetStatuses();
    nlohmann::json sessions = nlohmann::json::array();
    for (const auto& s : statuses) {
      sessions.push_back({{"session_id", s.session_id},
                          {"port", s.port},
                          {"connected", s.connected},
                          {"reconnecting", s.reconnecting},
                          {"last_error", s.last_error},
                          {"last_seen_ts", s.last_seen_ts},
                          {"unique_id", s.unique_id}});
    }

    WriteJson(res, {{"ok", true},
                    {"version", version_},
                    {"uptime_sec", uptime},
                    {"http", {{"host", host_}, {"port", port_}}},
                    {"ws", {{"host", host_}, {"port", ws_port_}, {"path", "/ws"}}},
                    {"active_sessions", sessions}});
  });
}

bool HttpServer::ParseJsonBody(const httplib::Request& req, nlohmann::json* out,
                               httplib::Response& res) const {
  if (out == nullptr) {
    WriteJson(res, {{"ok", false}, {"error", "INTERNAL_ERROR"}, {"message", "null output pointer"}}, 500);
    return false;
  }
  try {
    *out = nlohmann::json::parse(req.body);
    return true;
  } catch (...) {
    WriteJson(res, {{"ok", false}, {"error", "INVALID_JSON"}, {"message", "Request body is not valid JSON."}},
              400);
    return false;
  }
}

void HttpServer::ApplyCors(const httplib::Request& req, httplib::Response& res) const {
  const std::string origin = req.get_header_value("Origin");
  if (!origin.empty() && IsOriginAllowed(origin)) {
    res.set_header("Access-Control-Allow-Origin", origin.c_str());
    res.set_header("Vary", "Origin");
  }
  res.set_header("Access-Control-Allow-Headers", "Content-Type, X-Owner-Token");
  res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
}

bool HttpServer::IsOriginAllowed(const std::string& origin) const {
  if (allowed_origins_.empty()) {
    return false;
  }
  for (const auto& allowed : allowed_origins_) {
    if (allowed == "*" || allowed == origin) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> HttpServer::ParseCsv(const std::string& csv) {
  std::vector<std::string> out;
  std::size_t start = 0;
  while (start < csv.size()) {
    std::size_t comma = csv.find(',', start);
    if (comma == std::string::npos) {
      comma = csv.size();
    }
    const std::string item = Trim(csv.substr(start, comma - start));
    if (!item.empty()) {
      out.push_back(item);
    }
    start = comma + 1;
  }
  if (out.empty()) {
    out.push_back("http://localhost:3000");
    out.push_back("http://127.0.0.1:3000");
  }
  return out;
}

}  // namespace agent
