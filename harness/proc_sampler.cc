#include "harness/proc_sampler.h"

#include <fstream>
#include <string>
#include <sys/resource.h>

namespace rudp_bench {
namespace {

uint64_t cpu_us_now() {
  rusage r{};
  getrusage(RUSAGE_SELF, &r);
  uint64_t u = static_cast<uint64_t>(r.ru_utime.tv_sec) * 1'000'000ULL + r.ru_utime.tv_usec;
  uint64_t s = static_cast<uint64_t>(r.ru_stime.tv_sec) * 1'000'000ULL + r.ru_stime.tv_usec;
  return u + s;
}

uint64_t rss_kb_now() {
  std::ifstream f("/proc/self/status");
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("VmRSS:", 0) == 0) {
      uint64_t kb = 0;
      for (char c : line) if (c >= '0' && c <= '9') { kb = kb * 10 + (c - '0'); }
      return kb;
    }
  }
  return 0;
}

}  // namespace

void ProcSampler::begin() {
  t0_ = std::chrono::steady_clock::now();
  cpu_us_begin_ = cpu_us_now();
  last_cpu_t_ = t0_;
  last_cpu_us_ = cpu_us_begin_;
  cpu_pct_peak_ = 0.0;
  cpu_samples_ = 0;
  rss_mb_max_ = 0;
  rss_samples_ = 0;
  sample_rss();
}

void ProcSampler::mark_measure_begin() {
  // Discard everything observed before warmup_end: reset both the mean window
  // (t0_/cpu_us_begin_) and the per-interval peak baseline so warmup CPU is not
  // counted (M2).
  t0_ = std::chrono::steady_clock::now();
  cpu_us_begin_ = cpu_us_now();
  last_cpu_t_ = t0_;
  last_cpu_us_ = cpu_us_begin_;
  cpu_pct_peak_ = 0.0;
}

void ProcSampler::sample_rss() {
  uint64_t now = rss_kb_now() / 1024;
  if (now > rss_mb_max_) rss_mb_max_ = now;
  ++rss_samples_;
}

void ProcSampler::sample_cpu() {
  auto now_t = std::chrono::steady_clock::now();
  uint64_t now_cpu = cpu_us_now();
  uint64_t wall_us = std::chrono::duration_cast<std::chrono::microseconds>(
                         now_t - last_cpu_t_).count();
  if (wall_us > 0) {
    double pct = 100.0 * static_cast<double>(now_cpu - last_cpu_us_) /
                 static_cast<double>(wall_us);
    if (pct > cpu_pct_peak_) cpu_pct_peak_ = pct;
    ++cpu_samples_;
  }
  last_cpu_t_ = now_t;
  last_cpu_us_ = now_cpu;
}

void ProcSampler::end() {
  // Capture the final partial interval before closing the window.
  sample_cpu();
  t1_ = std::chrono::steady_clock::now();
  cpu_us_end_ = cpu_us_now();
  sample_rss();
}

double ProcSampler::cpu_pct() const {
  uint64_t cpu_us = cpu_us_end_ - cpu_us_begin_;
  uint64_t wall_us = std::chrono::duration_cast<std::chrono::microseconds>(t1_ - t0_).count();
  if (wall_us == 0) return 0.0;
  return 100.0 * static_cast<double>(cpu_us) / static_cast<double>(wall_us);
}

double ProcSampler::cpu_pct_peak() const {
  // Fall back to the mean if no periodic samples were taken.
  return cpu_pct_peak_ > 0.0 ? cpu_pct_peak_ : cpu_pct();
}

uint64_t ProcSampler::rss_max_mb() const { return rss_mb_max_; }

}  // namespace rudp_bench
