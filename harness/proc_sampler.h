#pragma once

#include <chrono>
#include <cstdint>

namespace rudp_bench {

class ProcSampler {
 public:
  void begin();
  // Re-baseline the CPU/wall accounting to "now" so warmup is excluded from
  // cpu_pct/cpu_pct_peak (M2). Safe to call once after warmup_end on either
  // role; if never called, accounting runs from begin().
  void mark_measure_begin();
  void sample_rss();
  // Periodic CPU sample: compute the CPU% of the interval since the last CPU
  // sample and track the peak (M1). The 2-point begin/end mean smears short
  // spikes; the peak surfaces them. Call on the same cadence as sample_rss().
  void sample_cpu();
  void end();
  double cpu_pct() const;        // mean (cpu_time / wall_time) * 100 over window
  double cpu_pct_peak() const;   // max per-interval cpu_pct seen via sample_cpu()
  uint64_t rss_max_mb() const;
  uint64_t rss_samples() const { return rss_samples_; }
  uint64_t cpu_samples() const { return cpu_samples_; }
 private:
  std::chrono::steady_clock::time_point t0_{};
  std::chrono::steady_clock::time_point t1_{};
  std::chrono::steady_clock::time_point last_cpu_t_{};
  uint64_t cpu_us_begin_ = 0;
  uint64_t cpu_us_end_ = 0;
  uint64_t last_cpu_us_ = 0;
  double cpu_pct_peak_ = 0.0;
  uint64_t rss_mb_max_ = 0;
  uint64_t rss_samples_ = 0;
  uint64_t cpu_samples_ = 0;
};

}  // namespace rudp_bench
