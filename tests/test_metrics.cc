#include <gtest/gtest.h>
#include "harness/metrics.h"

using namespace rudp_bench;

TEST(LatencyHist, Percentiles) {
  LatencyHist h;
  for (int i = 1; i <= 100; ++i) h.record_us(i);
  EXPECT_EQ(h.percentile_us(0.50), 50);
  EXPECT_EQ(h.percentile_us(0.95), 95);
  EXPECT_EQ(h.percentile_us(0.99), 99);
}

TEST(LatencyHist, EmptyReturnsZero) {
  LatencyHist h;
  EXPECT_EQ(h.percentile_us(0.50), 0);
}

TEST(DeliveryTracker, CountsAcceptedAndReceived) {
  DeliveryTracker d;
  d.mark_accepted(1, 0);
  d.mark_accepted(2, 0);
  d.mark_accepted(3, 0);
  d.mark_received(1, 0);
  d.mark_received(3, 0);
  EXPECT_EQ(d.accepted(), 3u);
  EXPECT_EQ(d.received(), 2u);
  EXPECT_DOUBLE_EQ(d.delivery_ratio(), 2.0 / 3.0);
}

TEST(ThroughputCounter, BytesAndMessages) {
  ThroughputCounter t;
  t.record(64);
  t.record(64);
  t.record(64);
  EXPECT_EQ(t.bytes(), 192u);
  EXPECT_EQ(t.messages(), 3u);
}
