#include <gtest/gtest.h>
#include "harness/proc_sampler.h"

#include <chrono>
#include <thread>
#include <sys/mman.h>
#include <unistd.h>

using namespace rudp_bench;

TEST(ProcSampler, CpuTimeIncreasesUnderBusyWork) {
  ProcSampler s;
  s.begin();
  // ~100ms ぶん CPU を回す
  auto t0 = std::chrono::steady_clock::now();
  volatile uint64_t x = 0;
  while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(100)) {
    for (int i = 0; i < 100000; ++i) x += i;
  }
  s.end();
  EXPECT_GT(s.cpu_pct(), 0.0);
}

TEST(ProcSampler, CpuPeakSurfacesSpikeAndMeasureBeginExcludesWarmup) {
  ProcSampler s;
  s.begin();
  // "warmup": ~80ms of busy CPU that must NOT count toward the measured window.
  auto burn = [](std::chrono::milliseconds d) {
    auto t0 = std::chrono::steady_clock::now();
    volatile uint64_t x = 0;
    while (std::chrono::steady_clock::now() - t0 < d) {
      for (int i = 0; i < 100000; ++i) x += i;
    }
    (void)x;
  };
  burn(std::chrono::milliseconds(80));
  s.sample_cpu();
  s.mark_measure_begin();  // M2: re-baseline; warmup CPU dropped from here

  // measured window: a short busy spike then idle, sampling between.
  burn(std::chrono::milliseconds(40));
  s.sample_cpu();
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  s.sample_cpu();
  s.end();

  // Peak (busy interval) must exceed the mean (busy+idle) — M1 surfaces spikes.
  EXPECT_GT(s.cpu_pct_peak(), 0.0);
  EXPECT_GE(s.cpu_pct_peak(), s.cpu_pct());
  EXPECT_GE(s.cpu_samples(), 2u);
}

TEST(ProcSampler, RssReadable) {
  ProcSampler s;
  s.begin();
  s.end();
  EXPECT_GT(s.rss_max_mb(), 0u);
  EXPECT_GE(s.rss_samples(), 2u);
}

TEST(ProcSampler, RssSampleCanObserveTransientAllocation) {
  ProcSampler s;
  s.begin();
  uint64_t baseline = s.rss_max_mb();

  const size_t bytes = 32 * 1024 * 1024;
  void* mem = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  ASSERT_NE(mem, MAP_FAILED);

  const long page_size = ::sysconf(_SC_PAGESIZE);
  ASSERT_GT(page_size, 0);
  auto* p = static_cast<volatile char*>(mem);
  for (size_t offset = 0; offset < bytes; offset += static_cast<size_t>(page_size)) {
    p[offset] = static_cast<char>(offset);
  }

  s.sample_rss();
  uint64_t during = s.rss_max_mb();
  EXPECT_GE(during, baseline + 8u);

  ASSERT_EQ(::munmap(mem, bytes), 0);
  s.end();
  EXPECT_GE(s.rss_max_mb(), during);
}
