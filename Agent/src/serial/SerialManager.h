#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <serialport.h>

#include "core/EventBus.h"
#include "serial/PortEnumerator.h"

namespace agent {

struct ValidateRequest {
  std::string port;
  int baud = 9600;
  std::vector<uint8_t> command_bytes;
  std::string expected_regex;
  int read_timeout_ms = 500;
  std::size_t max_read_bytes = 1024;
};

struct ValidateResult {
  bool ok = false;
  bool valid = false;
  std::string response_text;
  std::string response_b64;
  std::string details;
  std::string error_code;
  std::string error_message;
  int http_status = 200;
};

struct ConnectRequest {
  std::string port;
  int baud = 9600;
  std::string unique_id;
  std::string mode_type = "line";
  std::string delimiter = "\n";
};

struct ConnectResult {
  bool ok = false;
  std::string session_id;
  std::string port;
  std::string unique_id;
  std::string error_code;
  std::string error_message;
  int http_status = 200;
};

struct WriteResult {
  bool ok = false;
  int bytes_written = 0;
  std::string error_code;
  std::string error_message;
  int http_status = 200;
};

struct ReadResult {
  bool ok = false;
  std::vector<uint8_t> data;
  std::string data_b64;
  std::string data_text;
  std::size_t bytes = 0;
  std::string error_code;
  std::string error_message;
  int http_status = 200;
};

struct SessionStatus {
  std::string session_id;
  std::string port;
  bool connected = false;
  bool reconnecting = false;
  std::string last_error;
  std::int64_t last_seen_ts = 0;
  std::string unique_id;
};

class SerialManager {
 public:
  SerialManager(EventBus& event_bus, const PortEnumerator& enumerator);
  ~SerialManager();

  void Start();
  void Stop();

  void EnableMockSerial(bool enabled);

  std::vector<PortInfo> ListDevices() const;
  ValidateResult Validate(const ValidateRequest& req) const;

  ConnectResult Connect(const std::string& session_id, const ConnectRequest& req);
  bool Disconnect(const std::string& session_id, std::string* error_code = nullptr,
                  std::string* error_message = nullptr);

  WriteResult EnqueueWrite(const std::string& session_id, const std::vector<uint8_t>& bytes);
  ReadResult ReadBuffered(const std::string& session_id, int timeout_ms, std::size_t max_bytes);

  std::vector<SessionStatus> GetStatuses() const;

  static std::string EncodeBase64(const std::vector<uint8_t>& bytes);
  static bool DecodeBase64(const std::string& b64, std::vector<uint8_t>* out);
  static std::string BytesToText(const std::vector<uint8_t>& bytes);

 private:
  struct SerialSession {
    std::string session_id;
    std::string port;
    std::string unique_id;
    int baud = 9600;
    bool mock = false;

    std::string mode_type = "line";
    std::string delimiter = "\n";

    std::atomic<bool> running{true};
    std::atomic<bool> connected{false};
    std::atomic<bool> reconnecting{false};

    mutable std::mutex mutex;
    std::condition_variable data_cv;
    std::deque<std::vector<uint8_t>> write_queue;
    std::deque<uint8_t> read_buffer;
    std::string line_buffer;
    std::string last_error;
    std::int64_t last_seen_ts = 0;
    sp_port* handle = nullptr;
    std::thread worker;
  };

  bool IsPortInUseByAgent(const std::string& port) const;
  bool OpenPort(SerialSession& session, bool* port_in_use, std::string* error_message) const;
  void ClosePort(SerialSession& session) const;
  void WorkerLoop(const std::shared_ptr<SerialSession>& session);
  void MockWorkerLoop(const std::shared_ptr<SerialSession>& session);
  void MonitorPortsLoop();

  void PushReadData(const std::shared_ptr<SerialSession>& session, const uint8_t* data,
                    std::size_t size);
  void PublishSerialData(const std::shared_ptr<SerialSession>& session,
                         const std::vector<uint8_t>& payload) const;
  void PublishSimpleEvent(const std::string& type, const std::shared_ptr<SerialSession>& session,
                          const std::string& code = "", const std::string& message = "") const;
  static std::int64_t NowUnixSeconds();

  EventBus& event_bus_;
  const PortEnumerator& enumerator_;

  mutable std::mutex sessions_mutex_;
  std::unordered_map<std::string, std::shared_ptr<SerialSession>> sessions_;

  std::atomic<bool> monitor_running_{false};
  std::thread monitor_thread_;
  std::unordered_set<std::string> known_ports_;
  mutable std::mutex ports_mutex_;

  std::atomic<bool> mock_enabled_{false};
};

}  // namespace agent
