#include "core/SessionManager.h"

#include <random>
#include <sstream>

namespace agent {

SessionCredentials SessionManager::CreateSession() {
  SessionCredentials credentials;
  credentials.session_id = RandomHex(32);
  credentials.owner_token = RandomHex(48);

  std::lock_guard<std::mutex> lock(mutex_);
  session_owners_[credentials.session_id] = credentials.owner_token;
  return credentials;
}

void SessionManager::RemoveSession(const std::string& session_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  session_owners_.erase(session_id);
}

bool SessionManager::Exists(const std::string& session_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return session_owners_.find(session_id) != session_owners_.end();
}

bool SessionManager::ValidateOwner(const std::string& session_id, const std::string& owner_token) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = session_owners_.find(session_id);
  if (it == session_owners_.end()) {
    return false;
  }
  return it->second == owner_token;
}

std::string SessionManager::RandomHex(std::size_t hex_length) {
  static thread_local std::mt19937_64 rng(std::random_device{}());
  std::uniform_int_distribution<unsigned> dist(0, 15);

  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(hex_length);
  for (std::size_t i = 0; i < hex_length; ++i) {
    out.push_back(kHex[dist(rng)]);
  }
  return out;
}

}  // namespace agent
