#pragma once

#include <atomic>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace rudp_bench {

class LatencyHist {
 public:
  void record_us(uint64_t us);
  uint64_t percentile_us(double p);  // p in [0, 1]
  size_t samples() const { return samples_.size(); }
 private:
  std::vector<uint64_t> samples_;
  bool sorted_ = false;
};

class ThroughputCounter {
 public:
  void record(size_t bytes) { bytes_ += bytes; ++messages_; }
  uint64_t bytes() const { return bytes_; }
  uint64_t messages() const { return messages_; }
 private:
  uint64_t bytes_ = 0;
  uint64_t messages_ = 0;
};

class DeliveryTracker {
 public:
  void mark_sent(uint64_t seq, uint32_t conn_id);
  void mark_received(uint64_t seq, uint32_t conn_id);
  uint64_t sent() const { return sent_count_; }
  uint64_t received() const { return received_count_; }
  double delivery_ratio() const {
    return sent_count_ ? double(received_count_) / double(sent_count_) : 0.0;
  }
 private:
  uint64_t sent_count_ = 0;
  uint64_t received_count_ = 0;
  // 重複受信は数えない
  std::unordered_set<uint64_t> received_keys_;
};

}  // namespace rudp_bench
