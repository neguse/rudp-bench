#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include "harness/sliding_dedup_window.h"

namespace rudp_bench {

class LatencyHist {
 public:
  void record_us(uint64_t us);
  uint64_t percentile_us(double p);  // p in [0, 1]
  size_t samples() const { return static_cast<size_t>(count_); }
  size_t storage_bins() const { return bins_.size(); }
 private:
  static constexpr uint64_t kExactMaxUs = 10'000;
  static constexpr uint64_t kFineMaxUs = 1'000'000;
  static constexpr uint64_t kCoarseMaxUs = 60'000'000;
  static constexpr uint64_t kFineBinUs = 100;
  static constexpr uint64_t kCoarseBinUs = 1'000;
  static constexpr size_t kExactBins = kExactMaxUs + 1;
  static constexpr size_t kFineBins =
      (kFineMaxUs - kExactMaxUs + kFineBinUs - 1) / kFineBinUs;
  static constexpr size_t kCoarseBins =
      (kCoarseMaxUs - kFineMaxUs + kCoarseBinUs - 1) / kCoarseBinUs;
  static constexpr size_t kBinCount = kExactBins + kFineBins + kCoarseBins;

  size_t bin_index(uint64_t us) const;
  uint64_t bin_upper_bound_us(size_t index) const;

  std::array<uint64_t, kBinCount> bins_{};
  uint64_t count_ = 0;
  uint64_t overflow_ = 0;
  uint64_t max_ = 0;
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
  void mark_accepted(uint64_t seq, uint32_t conn_id);
  bool mark_received(uint64_t seq, uint32_t conn_id);
  uint64_t accepted() const { return accepted_count_; }
  uint64_t received() const { return received_count_; }
  size_t dedup_entries() const;
  static constexpr size_t dedup_window_per_conn() { return kDedupWindowPerConn; }
  static constexpr const char* dedup_policy() {
    return "sliding_window_65536_per_conn";
  }
  double delivery_ratio() const {
    return accepted_count_ ? double(received_count_) / double(accepted_count_) : 0.0;
  }
 private:
  static constexpr size_t kDedupWindowPerConn =
      SlidingDedupWindow::kDefaultLimit;
  static constexpr uint64_t kSeqMask = 0x0000FFFFFFFFFFFFULL;

  uint64_t accepted_count_ = 0;
  uint64_t received_count_ = 0;
  std::unordered_map<uint32_t, SlidingDedupWindow> received_by_conn_;
};

}  // namespace rudp_bench
