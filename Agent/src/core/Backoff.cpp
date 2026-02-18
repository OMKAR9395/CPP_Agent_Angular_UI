#include "core/Backoff.h"

#include <algorithm>

namespace agent {

Backoff::Backoff() : steps_ms_({500, 1000, 2000, 5000, 10000}), index_(0) {}

Backoff::Backoff(std::vector<int> steps_ms) : steps_ms_(std::move(steps_ms)), index_(0) {
  if (steps_ms_.empty()) {
    steps_ms_ = {500, 1000, 2000, 5000, 10000};
  }
}

std::chrono::milliseconds Backoff::NextDelay() {
  const std::size_t current = std::min(index_, steps_ms_.size() - 1);
  if (index_ < steps_ms_.size() - 1) {
    ++index_;
  }
  return std::chrono::milliseconds(steps_ms_[current]);
}

void Backoff::Reset() {
  index_ = 0;
}

}  // namespace agent
