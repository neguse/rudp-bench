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
  rss_mb_max_ = rss_kb_now() / 1024;
}

void ProcSampler::end() {
  t1_ = std::chrono::steady_clock::now();
  cpu_us_end_ = cpu_us_now();
  uint64_t now = rss_kb_now() / 1024;
  if (now > rss_mb_max_) rss_mb_max_ = now;
}

double ProcSampler::cpu_pct() const {
  uint64_t cpu_us = cpu_us_end_ - cpu_us_begin_;
  uint64_t wall_us = std::chrono::duration_cast<std::chrono::microseconds>(t1_ - t0_).count();
  if (wall_us == 0) return 0.0;
  return 100.0 * static_cast<double>(cpu_us) / static_cast<double>(wall_us);
}

uint64_t ProcSampler::rss_max_mb() const { return rss_mb_max_; }

}  // namespace rudp_bench
