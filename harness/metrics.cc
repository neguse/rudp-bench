#include "harness/metrics.h"

#include <algorithm>
#include <limits>

namespace rudp_bench {

size_t LatencyHist::bin_index(uint64_t us) const {
  if (us <= kExactMaxUs) return static_cast<size_t>(us);
  if (us <= kFineMaxUs) {
    return kExactBins + static_cast<size_t>((us - kExactMaxUs - 1) / kFineBinUs);
  }
  if (us <= kCoarseMaxUs) {
    return kExactBins + kFineBins +
           static_cast<size_t>((us - kFineMaxUs - 1) / kCoarseBinUs);
  }
  return std::numeric_limits<size_t>::max();
}

uint64_t LatencyHist::bin_upper_bound_us(size_t index) const {
  if (index < kExactBins) return static_cast<uint64_t>(index);
  if (index < kExactBins + kFineBins) {
    size_t fine_index = index - kExactBins;
    return kExactMaxUs + (static_cast<uint64_t>(fine_index) + 1) * kFineBinUs;
  }
  size_t coarse_index = index - kExactBins - kFineBins;
  return kFineMaxUs + (static_cast<uint64_t>(coarse_index) + 1) * kCoarseBinUs;
}

void LatencyHist::record_us(uint64_t us) {
  ++count_;
  max_ = std::max(max_, us);
  size_t index = bin_index(us);
  if (index == std::numeric_limits<size_t>::max()) {
    ++overflow_;
    return;
  }
  ++bins_[index];
}

uint64_t LatencyHist::percentile_us(double p) {
  if (count_ == 0) return 0;
  double q = std::clamp(p, 0.0, 1.0);
  uint64_t target = static_cast<uint64_t>(q * static_cast<double>(count_ - 1)) + 1;
  uint64_t seen = 0;
  for (size_t i = 0; i < bins_.size(); ++i) {
    seen += bins_[i];
    if (seen >= target) return bin_upper_bound_us(i);
  }
  (void)overflow_;
  return max_;
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
