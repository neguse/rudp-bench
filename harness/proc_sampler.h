#pragma once

#include <chrono>
#include <cstdint>

namespace rudp_bench {

class ProcSampler {
 public:
  void begin();
  void sample_rss();
  void end();
  double cpu_pct() const;        // (cpu_time / wall_time) * 100
  uint64_t rss_max_mb() const;
  uint64_t rss_samples() const { return rss_samples_; }
 private:
  std::chrono::steady_clock::time_point t0_{};
  std::chrono::steady_clock::time_point t1_{};
  uint64_t cpu_us_begin_ = 0;
  uint64_t cpu_us_end_ = 0;
  uint64_t rss_mb_max_ = 0;
  uint64_t rss_samples_ = 0;
};

}  // namespace rudp_bench
