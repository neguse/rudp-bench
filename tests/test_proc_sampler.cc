#include <gtest/gtest.h>
#include "harness/proc_sampler.h"

#include <chrono>
#include <thread>

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

TEST(ProcSampler, RssReadable) {
  ProcSampler s;
  s.begin();
  s.end();
  EXPECT_GT(s.rss_max_mb(), 0u);
}
