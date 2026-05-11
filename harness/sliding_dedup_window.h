#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_set>

namespace rudp_bench {

class SlidingDedupWindow {
 public:
  static constexpr size_t kDefaultLimit = 65'536;

  explicit SlidingDedupWindow(size_t limit = kDefaultLimit) : limit_(limit) {}

  bool insert(uint64_t key) {
    if (limit_ == 0) return true;
    if (!keys_.insert(key).second) return false;
    order_.push_back(key);
    if (order_.size() > limit_) {
      uint64_t old = order_.front();
      order_.pop_front();
      keys_.erase(old);
    }
    return true;
  }

  size_t size() const { return keys_.size(); }
  size_t limit() const { return limit_; }

 private:
  size_t limit_;
  std::deque<uint64_t> order_;
  std::unordered_set<uint64_t> keys_;
};

}  // namespace rudp_bench
