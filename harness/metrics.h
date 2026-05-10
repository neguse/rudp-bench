#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_set>

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
  double delivery_ratio() const {
    return accepted_count_ ? double(received_count_) / double(accepted_count_) : 0.0;
  }
 private:
  uint64_t accepted_count_ = 0;
  uint64_t received_count_ = 0;
  // 重複受信は数えない
  std::unordered_set<uint64_t> received_keys_;
};

}  // namespace rudp_bench
