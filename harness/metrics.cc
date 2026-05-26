#include "harness/metrics.h"

#include <algorithm>
#include <limits>
#include <ostream>

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

void LatencyHist::write_binary(std::ostream& os) const {
  // dense u64 little-endian payload, header:
  //   u32 magic = 'LHST' (0x5453484c)
  //   u32 version = 1
  //   u64 count, overflow, max_us, bin_count
  //   u64 bins[bin_count]
  const uint32_t magic = 0x5453484CU;
  const uint32_t version = 1;
  const uint64_t bin_count_field = kBinCount;
  os.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
  os.write(reinterpret_cast<const char*>(&version), sizeof(version));
  os.write(reinterpret_cast<const char*>(&count_), sizeof(count_));
  os.write(reinterpret_cast<const char*>(&overflow_), sizeof(overflow_));
  os.write(reinterpret_cast<const char*>(&max_), sizeof(max_));
  os.write(reinterpret_cast<const char*>(&bin_count_field), sizeof(bin_count_field));
  os.write(reinterpret_cast<const char*>(bins_.data()),
           static_cast<std::streamsize>(sizeof(uint64_t) * kBinCount));
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

void DeliveryTracker::mark_accepted(uint64_t seq, uint32_t conn_id) {
  ++accepted_count_;
  (void)seq;
  (void)conn_id;
}

bool DeliveryTracker::mark_received(uint64_t seq, uint32_t conn_id) {
  uint64_t k = seq & kSeqMask;
  auto& window = received_by_conn_[conn_id];
  if (!window.insert(k)) return false;
  ++received_count_;
  return true;
}

size_t DeliveryTracker::dedup_entries() const {
  size_t entries = 0;
  for (const auto& [conn_id, window] : received_by_conn_) {
    (void)conn_id;
    entries += window.size();
  }
  return entries;
}

}  // namespace rudp_bench
