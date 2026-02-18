#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace agent {

struct SessionCredentials {
  std::string session_id;
  std::string owner_token;
};

class SessionManager {
 public:
  SessionCredentials CreateSession();
  void RemoveSession(const std::string& session_id);

  bool Exists(const std::string& session_id) const;
  bool ValidateOwner(const std::string& session_id, const std::string& owner_token) const;

 private:
  static std::string RandomHex(std::size_t hex_length);

  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::string> session_owners_;
};

}  // namespace agent
