#pragma once

#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <unordered_map>
#include <vector>

namespace agent {

class EventBus {
 public:
  using Event = nlohmann::json;
  using Callback = std::function<void(const Event&)>;

  int Subscribe(Callback cb);
  void Unsubscribe(int subscription_id);
  void Publish(const Event& event);

 private:
  mutable std::mutex mutex_;
  int next_subscription_id_ = 1;
  std::unordered_map<int, Callback> subscribers_;
};

}  // namespace agent
