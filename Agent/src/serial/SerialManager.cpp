#include "serial/SerialManager.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <regex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "core/Backoff.h"

namespace agent {
namespace {

constexpr std::size_t kMaxBufferedBytes = 1024 * 1024;

std::string ToLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string ConsumeLastSerialError() {
  char* msg = sp_last_error_message();
  if (msg == nullptr) {
    return {};
  }
  std::string out(msg);
  sp_free_error_message(msg);
  return out;
}

bool LooksLikePortInUse(const std::string& message) {
  const std::string lower = ToLower(message);
  return lower.find("busy") != std::string::npos || lower.find("in use") != std::string::npos ||
         lower.find("access is denied") != std::string::npos ||
         lower.find("permission denied") != std::string::npos;
}

bool ConfigurePort(sp_port* port, int baud, std::string* err) {
  if (sp_set_baudrate(port, baud) != SP_OK) {
    if (err != nullptr) {
      *err = ConsumeLastSerialError();
    }
    return false;
  }
  if (sp_set_bits(port, 8) != SP_OK) {
    if (err != nullptr) {
      *err = ConsumeLastSerialError();
    }
    return false;
  }
  if (sp_set_parity(port, SP_PARITY_NONE) != SP_OK) {
    if (err != nullptr) {
      *err = ConsumeLastSerialError();
    }
    return false;
  }
  if (sp_set_stopbits(port, 1) != SP_OK) {
    if (err != nullptr) {
      *err = ConsumeLastSerialError();
    }
    return false;
  }
  if (sp_set_flowcontrol(port, SP_FLOWCONTROL_NONE) != SP_OK) {
    if (err != nullptr) {
      *err = ConsumeLastSerialError();
    }
    return false;
  }
  return true;
}

}  // namespace

SerialManager::SerialManager(EventBus& event_bus, const PortEnumerator& enumerator)
    : event_bus_(event_bus), enumerator_(enumerator) {}

SerialManager::~SerialManager() {
  Stop();
}

void SerialManager::Start() {
  if (monitor_running_.exchange(true)) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(ports_mutex_);
    known_ports_.clear();
    for (const auto& port : enumerator_.ListPorts(false)) {
      known_ports_.insert(port.port);
    }
    if (mock_enabled_.load()) {
      known_ports_.insert("MOCK0");
    }
  }

  monitor_thread_ = std::thread(&SerialManager::MonitorPortsLoop, this);
}

void SerialManager::Stop() {
  monitor_running_.store(false);
  if (monitor_thread_.joinable()) {
    monitor_thread_.join();
  }

  std::vector<std::string> session_ids;
  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    session_ids.reserve(sessions_.size());
    for (const auto& [id, _] : sessions_) {
      session_ids.push_back(id);
    }
  }

  for (const auto& session_id : session_ids) {
    Disconnect(session_id);
  }
}

void SerialManager::EnableMockSerial(bool enabled) {
  mock_enabled_.store(enabled);
}

std::vector<PortInfo> SerialManager::ListDevices() const {
  auto ports = enumerator_.ListPorts(true);

  for (auto& port : ports) {
    if (IsPortInUseByAgent(port.port)) {
      port.is_busy = true;
      if (!port.notes.empty()) {
        port.notes += " ";
      }
      port.notes += "Port is currently opened by this agent.";
    }
  }

  if (mock_enabled_.load()) {
    PortInfo mock;
    mock.port = "MOCK0";
    mock.display_name = "Mock Serial Device";
    mock.unique_id = "mock:device:0";
    mock.is_busy = IsPortInUseByAgent("MOCK0");
    mock.notes = "Synthetic test device. Emits TEST123\\n once per second.";
    ports.push_back(std::move(mock));
  }

  return ports;
}

ValidateResult SerialManager::Validate(const ValidateRequest& req) const {
  ValidateResult out;
  if (req.port.empty()) {
    out.http_status = 400;
    out.error_code = "INVALID_REQUEST";
    out.error_message = "Field 'port' is required.";
    return out;
  }

  if (IsPortInUseByAgent(req.port)) {
    out.http_status = 409;
    out.error_code = "PORT_IN_USE";
    out.error_message = "Port is being used by another application.";
    return out;
  }

  if (mock_enabled_.load() && req.port == "MOCK0") {
    const std::vector<uint8_t> response = {'T', 'E', 'S', 'T', '1', '2', '3', '\n'};
    out.response_text = BytesToText(response);
    out.response_b64 = EncodeBase64(response);
    out.details = "Mock validate response";
    try {
      if (req.expected_regex.empty()) {
        out.valid = true;
      } else {
        const std::regex rx(req.expected_regex);
        out.valid = std::regex_search(out.response_text, rx);
      }
    } catch (const std::regex_error&) {
      out.http_status = 400;
      out.error_code = "INVALID_REGEX";
      out.error_message = "expected_regex is invalid.";
      return out;
    }
    out.ok = true;
    return out;
  }

  sp_port* port = nullptr;
  bool opened = false;
  const auto cleanup = [&]() {
    if (port != nullptr) {
      if (opened) {
        sp_close(port);
      }
      sp_free_port(port);
      port = nullptr;
    }
  };

  if (sp_get_port_by_name(req.port.c_str(), &port) != SP_OK || port == nullptr) {
    out.http_status = 404;
    out.error_code = "PORT_NOT_FOUND";
    out.error_message = "Port not found.";
    cleanup();
    return out;
  }

  if (sp_open(port, SP_MODE_READ_WRITE) != SP_OK) {
    const std::string err = ConsumeLastSerialError();
    out.http_status = LooksLikePortInUse(err) ? 409 : 500;
    out.error_code = LooksLikePortInUse(err) ? "PORT_IN_USE" : "IO_ERROR";
    out.error_message =
        LooksLikePortInUse(err) ? "Port is being used by another application."
                                : (err.empty() ? "Failed to open port." : err);
    cleanup();
    return out;
  }
  opened = true;

  std::string config_err;
  if (!ConfigurePort(port, req.baud, &config_err)) {
    out.http_status = 500;
    out.error_code = "IO_ERROR";
    out.error_message = config_err.empty() ? "Failed to configure serial port." : config_err;
    cleanup();
    return out;
  }

  if (!req.command_bytes.empty()) {
    int offset = 0;
    while (offset < static_cast<int>(req.command_bytes.size())) {
      const int wrote = sp_blocking_write(
          port, req.command_bytes.data() + offset,
          static_cast<int>(req.command_bytes.size()) - offset,
          static_cast<unsigned int>(std::max(100, req.read_timeout_ms)));
      if (wrote < 0) {
        out.http_status = 500;
        out.error_code = "IO_ERROR";
        const std::string err = ConsumeLastSerialError();
        out.error_message = err.empty() ? "Failed writing validate command." : err;
        cleanup();
        return out;
      }
      offset += wrote;
    }
  }

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(req.read_timeout_ms);
  std::vector<uint8_t> response;
  response.reserve(req.max_read_bytes);
  while (response.size() < req.max_read_bytes) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      break;
    }

    const auto remaining_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    if (remaining_ms <= 0) {
      break;
    }

    std::array<uint8_t, 256> chunk{};
    const std::size_t want = std::min(chunk.size(), req.max_read_bytes - response.size());
    const int read_rc =
        sp_blocking_read(port, chunk.data(), static_cast<int>(want), static_cast<unsigned>(remaining_ms));

    if (read_rc > 0) {
      response.insert(response.end(), chunk.begin(), chunk.begin() + read_rc);
      continue;
    }
    if (read_rc == 0) {
      break;
    }

    out.http_status = 500;
    out.error_code = "IO_ERROR";
    const std::string err = ConsumeLastSerialError();
    out.error_message = err.empty() ? "Failed while reading validate response." : err;
    cleanup();
    return out;
  }

  cleanup();

  out.response_text = BytesToText(response);
  out.response_b64 = EncodeBase64(response);
  out.details = "Validation completed.";

  try {
    if (req.expected_regex.empty()) {
      out.valid = true;
    } else {
      const std::regex rx(req.expected_regex);
      out.valid = std::regex_search(out.response_text, rx);
    }
  } catch (const std::regex_error&) {
    out.http_status = 400;
    out.error_code = "INVALID_REGEX";
    out.error_message = "expected_regex is invalid.";
    return out;
  }

  out.ok = true;
  return out;
}

ConnectResult SerialManager::Connect(const std::string& session_id, const ConnectRequest& req) {
  ConnectResult out;
  out.port = req.port;
  out.session_id = session_id;

  if (session_id.empty() || req.port.empty()) {
    out.http_status = 400;
    out.error_code = "INVALID_REQUEST";
    out.error_message = "session_id and port are required.";
    return out;
  }

  if (req.mode_type != "line" && req.mode_type != "raw") {
    out.http_status = 400;
    out.error_code = "INVALID_MODE";
    out.error_message = "mode.type must be 'line' or 'raw'.";
    return out;
  }

  if (IsPortInUseByAgent(req.port)) {
    out.http_status = 409;
    out.error_code = "PORT_IN_USE";
    out.error_message = "Port is being used by another application.";
    return out;
  }

  std::string derived_unique_id;
  const bool is_mock = mock_enabled_.load() && req.port == "MOCK0";
  if (!is_mock) {
    const auto port_info = enumerator_.FindPort(req.port, false);
    if (!port_info.has_value()) {
      out.http_status = 404;
      out.error_code = "PORT_NOT_FOUND";
      out.error_message = "Port not found.";
      return out;
    }
    derived_unique_id = port_info->unique_id;
  } else {
    derived_unique_id = "mock:device:0";
  }

  if (!req.unique_id.empty() && !derived_unique_id.empty() && req.unique_id != derived_unique_id) {
    out.http_status = 400;
    out.error_code = "UNIQUE_ID_MISMATCH";
    out.error_message = "Provided unique_id does not match the selected port.";
    return out;
  }

  auto session = std::make_shared<SerialSession>();
  session->session_id = session_id;
  session->port = req.port;
  session->baud = req.baud;
  session->mode_type = req.mode_type;
  session->delimiter = req.delimiter.empty() ? "\n" : req.delimiter;
  session->unique_id = derived_unique_id;
  session->mock = is_mock;
  session->last_seen_ts = NowUnixSeconds();

  if (!is_mock) {
    bool port_in_use = false;
    std::string open_err;
    if (!OpenPort(*session, &port_in_use, &open_err)) {
      out.http_status = port_in_use ? 409 : 500;
      out.error_code = port_in_use ? "PORT_IN_USE" : "IO_ERROR";
      out.error_message = port_in_use ? "Port is being used by another application."
                                      : (open_err.empty() ? "Failed to open port." : open_err);
      return out;
    }
    session->connected.store(true);
  } else {
    session->connected.store(true);
  }

  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    if (sessions_.find(session_id) != sessions_.end()) {
      ClosePort(*session);
      out.http_status = 409;
      out.error_code = "SESSION_ALREADY_EXISTS";
      out.error_message = "Session id already exists.";
      return out;
    }
    sessions_[session_id] = session;
  }

  if (session->mock) {
    session->worker = std::thread(&SerialManager::MockWorkerLoop, this, session);
  } else {
    session->worker = std::thread(&SerialManager::WorkerLoop, this, session);
  }

  out.ok = true;
  out.unique_id = derived_unique_id;
  PublishSimpleEvent("serial_connected", session);
  return out;
}

bool SerialManager::Disconnect(const std::string& session_id, std::string* error_code,
                               std::string* error_message) {
  std::shared_ptr<SerialSession> session;
  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
      if (error_code != nullptr) {
        *error_code = "SESSION_NOT_FOUND";
      }
      if (error_message != nullptr) {
        *error_message = "session_id was not found.";
      }
      return false;
    }
    session = it->second;
    sessions_.erase(it);
  }

  session->running.store(false);
  session->data_cv.notify_all();

  if (session->worker.joinable()) {
    session->worker.join();
  }

  ClosePort(*session);
  PublishSimpleEvent("serial_disconnected", session);
  return true;
}

WriteResult SerialManager::EnqueueWrite(const std::string& session_id, const std::vector<uint8_t>& bytes) {
  WriteResult out;
  std::shared_ptr<SerialSession> session;
  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
      out.http_status = 404;
      out.error_code = "SESSION_NOT_FOUND";
      out.error_message = "session_id was not found.";
      return out;
    }
    session = it->second;
  }

  {
    std::lock_guard<std::mutex> lock(session->mutex);
    if (!session->running.load()) {
      out.http_status = 409;
      out.error_code = "SESSION_CLOSED";
      out.error_message = "Session is already closed.";
      return out;
    }
    session->write_queue.push_back(bytes);
  }

  out.ok = true;
  out.bytes_written = static_cast<int>(bytes.size());
  return out;
}

ReadResult SerialManager::ReadBuffered(const std::string& session_id, int timeout_ms, std::size_t max_bytes) {
  ReadResult out;
  std::shared_ptr<SerialSession> session;
  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it == sessions_.end()) {
      out.http_status = 404;
      out.error_code = "SESSION_NOT_FOUND";
      out.error_message = "session_id was not found.";
      return out;
    }
    session = it->second;
  }

  std::vector<uint8_t> payload;
  {
    std::unique_lock<std::mutex> lock(session->mutex);
    if (session->read_buffer.empty() && timeout_ms > 0) {
      session->data_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&]() {
        return !session->read_buffer.empty() || !session->running.load();
      });
    }

    const std::size_t n = std::min(max_bytes, session->read_buffer.size());
    payload.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
      payload.push_back(session->read_buffer.front());
      session->read_buffer.pop_front();
    }
  }

  out.ok = true;
  out.data = payload;
  out.bytes = payload.size();
  out.data_b64 = EncodeBase64(payload);
  out.data_text = BytesToText(payload);
  return out;
}

std::vector<SessionStatus> SerialManager::GetStatuses() const {
  std::vector<std::shared_ptr<SerialSession>> sessions;
  {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    sessions.reserve(sessions_.size());
    for (const auto& [_, session] : sessions_) {
      sessions.push_back(session);
    }
  }

  std::vector<SessionStatus> out;
  out.reserve(sessions.size());
  for (const auto& session : sessions) {
    SessionStatus status;
    status.session_id = session->session_id;
    status.port = session->port;
    status.unique_id = session->unique_id;
    status.connected = session->connected.load();
    status.reconnecting = session->reconnecting.load();
    {
      std::lock_guard<std::mutex> lock(session->mutex);
      status.last_error = session->last_error;
      status.last_seen_ts = session->last_seen_ts;
    }
    out.push_back(std::move(status));
  }

  return out;
}

std::string SerialManager::EncodeBase64(const std::vector<uint8_t>& bytes) {
  static constexpr char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  std::string out;
  out.reserve(((bytes.size() + 2) / 3) * 4);

  std::size_t i = 0;
  while (i + 2 < bytes.size()) {
    const uint32_t n = (static_cast<uint32_t>(bytes[i]) << 16U) |
                       (static_cast<uint32_t>(bytes[i + 1]) << 8U) |
                       static_cast<uint32_t>(bytes[i + 2]);
    out.push_back(kTable[(n >> 18U) & 63U]);
    out.push_back(kTable[(n >> 12U) & 63U]);
    out.push_back(kTable[(n >> 6U) & 63U]);
    out.push_back(kTable[n & 63U]);
    i += 3;
  }

  const std::size_t rem = bytes.size() - i;
  if (rem == 1) {
    const uint32_t n = static_cast<uint32_t>(bytes[i]) << 16U;
    out.push_back(kTable[(n >> 18U) & 63U]);
    out.push_back(kTable[(n >> 12U) & 63U]);
    out.push_back('=');
    out.push_back('=');
  } else if (rem == 2) {
    const uint32_t n =
        (static_cast<uint32_t>(bytes[i]) << 16U) | (static_cast<uint32_t>(bytes[i + 1]) << 8U);
    out.push_back(kTable[(n >> 18U) & 63U]);
    out.push_back(kTable[(n >> 12U) & 63U]);
    out.push_back(kTable[(n >> 6U) & 63U]);
    out.push_back('=');
  }

  return out;
}

bool SerialManager::DecodeBase64(const std::string& b64, std::vector<uint8_t>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();

  std::array<int, 256> table{};
  table.fill(-1);
  for (int i = 0; i < 26; ++i) {
    table[static_cast<unsigned char>('A' + i)] = i;
    table[static_cast<unsigned char>('a' + i)] = i + 26;
  }
  for (int i = 0; i < 10; ++i) {
    table[static_cast<unsigned char>('0' + i)] = i + 52;
  }
  table[static_cast<unsigned char>('+')] = 62;
  table[static_cast<unsigned char>('/')] = 63;

  int val = 0;
  int valb = -8;
  for (unsigned char c : b64) {
    if (std::isspace(c)) {
      continue;
    }
    if (c == '=') {
      break;
    }
    const int d = table[c];
    if (d == -1) {
      return false;
    }
    val = (val << 6) + d;
    valb += 6;
    if (valb >= 0) {
      out->push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
      valb -= 8;
    }
  }
  return true;
}

std::string SerialManager::BytesToText(const std::vector<uint8_t>& bytes) {
  std::string text;
  text.reserve(bytes.size());
  for (const uint8_t b : bytes) {
    if (b == '\n' || b == '\r' || b == '\t' || (b >= 32 && b <= 126)) {
      text.push_back(static_cast<char>(b));
    } else {
      text.push_back('.');
    }
  }
  return text;
}

bool SerialManager::IsPortInUseByAgent(const std::string& port) const {
  std::lock_guard<std::mutex> lock(sessions_mutex_);
  for (const auto& [_, session] : sessions_) {
    if (session->port == port && session->running.load()) {
      return true;
    }
  }
  return false;
}

bool SerialManager::OpenPort(SerialSession& session, bool* port_in_use, std::string* error_message) const {
  if (port_in_use != nullptr) {
    *port_in_use = false;
  }
  if (error_message != nullptr) {
    error_message->clear();
  }

  sp_port* port = nullptr;
  if (sp_get_port_by_name(session.port.c_str(), &port) != SP_OK || port == nullptr) {
    if (error_message != nullptr) {
      *error_message = "Port not found.";
    }
    return false;
  }

  if (sp_open(port, SP_MODE_READ_WRITE) != SP_OK) {
    const std::string err = ConsumeLastSerialError();
    if (port_in_use != nullptr) {
      *port_in_use = LooksLikePortInUse(err);
    }
    if (error_message != nullptr) {
      *error_message = err.empty() ? "Failed to open port." : err;
    }
    sp_free_port(port);
    return false;
  }

  std::string config_err;
  if (!ConfigurePort(port, session.baud, &config_err)) {
    if (error_message != nullptr) {
      *error_message = config_err.empty() ? "Failed to configure serial port." : config_err;
    }
    sp_close(port);
    sp_free_port(port);
    return false;
  }

  session.handle = port;
  return true;
}

void SerialManager::ClosePort(SerialSession& session) const {
  if (session.handle != nullptr) {
    sp_close(session.handle);
    sp_free_port(session.handle);
    session.handle = nullptr;
  }
}

void SerialManager::WorkerLoop(const std::shared_ptr<SerialSession>& session) {
  Backoff backoff;
  bool reconnect_notified = false;

  while (session->running.load()) {
    if (!session->connected.load()) {
      bool port_in_use = false;
      std::string err;
      if (OpenPort(*session, &port_in_use, &err)) {
        session->connected.store(true);
        session->reconnecting.store(false);
        {
          std::lock_guard<std::mutex> lock(session->mutex);
          session->last_error.clear();
          session->last_seen_ts = NowUnixSeconds();
        }
        backoff.Reset();
        reconnect_notified = false;
        PublishSimpleEvent("serial_connected", session);
      } else {
        session->reconnecting.store(true);
        {
          std::lock_guard<std::mutex> lock(session->mutex);
          session->last_error = err.empty() ? "Reconnect failed." : err;
        }
        if (!reconnect_notified) {
          PublishSimpleEvent("serial_reconnecting", session, port_in_use ? "PORT_IN_USE" : "IO_ERROR",
                             err);
          reconnect_notified = true;
        }
        std::this_thread::sleep_for(backoff.NextDelay());
      }
      continue;
    }

    std::vector<uint8_t> write_chunk;
    {
      std::lock_guard<std::mutex> lock(session->mutex);
      if (!session->write_queue.empty()) {
        write_chunk = std::move(session->write_queue.front());
        session->write_queue.pop_front();
      }
    }

    if (!write_chunk.empty()) {
      int offset = 0;
      while (offset < static_cast<int>(write_chunk.size())) {
        const int wrote = sp_blocking_write(session->handle, write_chunk.data() + offset,
                                            static_cast<int>(write_chunk.size()) - offset, 500);
        if (wrote < 0) {
          const std::string err = ConsumeLastSerialError();
          session->connected.store(false);
          session->reconnecting.store(true);
          {
            std::lock_guard<std::mutex> lock(session->mutex);
            session->last_error = err.empty() ? "Write failed." : err;
          }
          ClosePort(*session);
          PublishSimpleEvent("serial_disconnected", session, "IO_ERROR", err);
          PublishSimpleEvent("serial_reconnecting", session, "IO_ERROR", err);
          reconnect_notified = true;
          break;
        }
        offset += wrote;
      }
      if (!session->connected.load()) {
        continue;
      }
    }

    std::array<uint8_t, 512> buf{};
    const int read_rc = sp_blocking_read(session->handle, buf.data(), static_cast<int>(buf.size()), 100);
    if (read_rc > 0) {
      PushReadData(session, buf.data(), static_cast<std::size_t>(read_rc));
      continue;
    }
    if (read_rc == 0) {
      continue;
    }

    const std::string err = ConsumeLastSerialError();
    session->connected.store(false);
    session->reconnecting.store(true);
    {
      std::lock_guard<std::mutex> lock(session->mutex);
      session->last_error = err.empty() ? "Read failed." : err;
    }
    ClosePort(*session);
    PublishSimpleEvent("serial_disconnected", session, "IO_ERROR", err);
    PublishSimpleEvent("serial_reconnecting", session, "IO_ERROR", err);
    reconnect_notified = true;
  }

  ClosePort(*session);
  session->connected.store(false);
  session->reconnecting.store(false);
}

void SerialManager::MockWorkerLoop(const std::shared_ptr<SerialSession>& session) {
  using namespace std::chrono_literals;

  auto next_tick = std::chrono::steady_clock::now() + 1s;
  while (session->running.load()) {
    std::vector<uint8_t> write_chunk;
    {
      std::lock_guard<std::mutex> lock(session->mutex);
      if (!session->write_queue.empty()) {
        write_chunk = std::move(session->write_queue.front());
        session->write_queue.pop_front();
      }
    }

    if (!write_chunk.empty()) {
      PushReadData(session, write_chunk.data(), write_chunk.size());
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= next_tick) {
      const std::string sample = "TEST123\n";
      PushReadData(session, reinterpret_cast<const uint8_t*>(sample.data()), sample.size());
      next_tick = now + 1s;
    }
    std::this_thread::sleep_for(50ms);
  }
}

void SerialManager::MonitorPortsLoop() {
  using namespace std::chrono_literals;

  while (monitor_running_.load()) {
    std::unordered_set<std::string> current;
    for (const auto& port : enumerator_.ListPorts(false)) {
      current.insert(port.port);
    }
    if (mock_enabled_.load()) {
      current.insert("MOCK0");
    }

    std::vector<std::string> attached;
    std::vector<std::string> detached;
    {
      std::lock_guard<std::mutex> lock(ports_mutex_);
      for (const auto& p : current) {
        if (known_ports_.find(p) == known_ports_.end()) {
          attached.push_back(p);
        }
      }
      for (const auto& p : known_ports_) {
        if (current.find(p) == current.end()) {
          detached.push_back(p);
        }
      }
      known_ports_ = std::move(current);
    }

    for (const auto& port : attached) {
      event_bus_.Publish({{"type", "device_attached"}, {"port", port}, {"ts", NowUnixSeconds()}});
    }
    for (const auto& port : detached) {
      event_bus_.Publish({{"type", "device_detached"}, {"port", port}, {"ts", NowUnixSeconds()}});
    }

    for (int i = 0; i < 10 && monitor_running_.load(); ++i) {
      std::this_thread::sleep_for(100ms);
    }
  }
}

void SerialManager::PushReadData(const std::shared_ptr<SerialSession>& session, const uint8_t* data,
                                 std::size_t size) {
  std::vector<std::vector<uint8_t>> lines;
  std::vector<uint8_t> raw_chunk(data, data + size);

  {
    std::lock_guard<std::mutex> lock(session->mutex);

    for (std::size_t i = 0; i < size; ++i) {
      if (session->read_buffer.size() >= kMaxBufferedBytes) {
        session->read_buffer.pop_front();
      }
      session->read_buffer.push_back(data[i]);
    }
    session->last_seen_ts = NowUnixSeconds();
    session->data_cv.notify_all();

    if (session->mode_type == "line") {
      session->line_buffer.append(reinterpret_cast<const char*>(data), size);
      const std::string& delimiter = session->delimiter.empty() ? std::string("\n") : session->delimiter;

      std::size_t pos = std::string::npos;
      while ((pos = session->line_buffer.find(delimiter)) != std::string::npos) {
        const std::size_t end = pos + delimiter.size();
        const std::string line = session->line_buffer.substr(0, end);
        session->line_buffer.erase(0, end);
        lines.emplace_back(line.begin(), line.end());
      }
    }
  }

  if (session->mode_type == "line") {
    for (const auto& line : lines) {
      PublishSerialData(session, line);
    }
  } else {
    PublishSerialData(session, raw_chunk);
  }
}

void SerialManager::PublishSerialData(const std::shared_ptr<SerialSession>& session,
                                      const std::vector<uint8_t>& payload) const {
  event_bus_.Publish({{"type", "serial_data"},
                      {"session_id", session->session_id},
                      {"port", session->port},
                      {"text", BytesToText(payload)},
                      {"b64", EncodeBase64(payload)},
                      {"ts", NowUnixSeconds()}});
}

void SerialManager::PublishSimpleEvent(const std::string& type, const std::shared_ptr<SerialSession>& session,
                                       const std::string& code, const std::string& message) const {
  nlohmann::json event = {{"type", type},
                          {"session_id", session->session_id},
                          {"port", session->port},
                          {"ts", NowUnixSeconds()}};
  if (!code.empty()) {
    event["code"] = code;
  }
  if (!message.empty()) {
    event["message"] = message;
  }
  event_bus_.Publish(event);

  if (!code.empty()) {
    nlohmann::json error_event = {{"type", "error"},
                                  {"code", code},
                                  {"message", message.empty() ? type : message},
                                  {"session_id", session->session_id},
                                  {"port", session->port},
                                  {"ts", NowUnixSeconds()}};
    event_bus_.Publish(error_event);
  }
}

std::int64_t SerialManager::NowUnixSeconds() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace agent
