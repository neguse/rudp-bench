#include "harness/metrics.h"

#include <algorithm>

namespace rudp_bench {

void LatencyHist::record_us(uint64_t us) {
  samples_.push_back(us);
  sorted_ = false;
}

uint64_t LatencyHist::percentile_us(double p) {
  if (samples_.empty()) return 0;
  if (!sorted_) {
    std::sort(samples_.begin(), samples_.end());
    sorted_ = true;
  }
  size_t idx = static_cast<size_t>(p * (samples_.size() - 1));
  return samples_[idx];
}

static uint64_t pack(uint64_t seq, uint32_t conn_id) {
  return (static_cast<uint64_t>(conn_id) << 48) | (seq & 0x0000FFFFFFFFFFFFULL);
}

void DeliveryTracker::mark_accepted(uint64_t seq, uint32_t conn_id) {
  ++accepted_count_;
  (void)pack(seq, conn_id);  // 受理側は count のみ管理
}

bool DeliveryTracker::mark_received(uint64_t seq, uint32_t conn_id) {
  uint64_t k = pack(seq, conn_id);
  if (!received_keys_.insert(k).second) return false;
  ++received_count_;
  return true;
}

}  // namespace rudp_bench
