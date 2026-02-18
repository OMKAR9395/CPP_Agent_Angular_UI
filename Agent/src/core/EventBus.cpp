#include "core/EventBus.h"

namespace agent {

int EventBus::Subscribe(Callback cb) {
  std::lock_guard<std::mutex> lock(mutex_);
  const int id = next_subscription_id_++;
  subscribers_[id] = std::move(cb);
  return id;
}

void EventBus::Unsubscribe(int subscription_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  subscribers_.erase(subscription_id);
}

void EventBus::Publish(const Event& event) {
  std::vector<Callback> callbacks;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks.reserve(subscribers_.size());
    for (const auto& [_, callback] : subscribers_) {
      callbacks.push_back(callback);
    }
  }

  for (const auto& callback : callbacks) {
    if (callback) {
      callback(event);
    }
  }
}

}  // namespace agent
