#pragma once

#include <chrono>
#include <cstddef>
#include <vector>

namespace agent {

class Backoff {
 public:
  Backoff();
  explicit Backoff(std::vector<int> steps_ms);

  std::chrono::milliseconds NextDelay();
  void Reset();

 private:
  std::vector<int> steps_ms_;
  std::size_t index_;
};

}  // namespace agent
