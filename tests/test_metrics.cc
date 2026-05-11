#include <gtest/gtest.h>
#include "harness/metrics.h"
#include "harness/sliding_dedup_window.h"

using namespace rudp_bench;

TEST(LatencyHist, Percentiles) {
  LatencyHist h;
  for (int i = 1; i <= 100; ++i) h.record_us(i);
  EXPECT_EQ(h.percentile_us(0.50), 50);
  EXPECT_EQ(h.percentile_us(0.95), 95);
  EXPECT_EQ(h.percentile_us(0.99), 99);
  EXPECT_EQ(h.samples(), 100u);
}

TEST(LatencyHist, EmptyReturnsZero) {
  LatencyHist h;
  EXPECT_EQ(h.percentile_us(0.50), 0);
}

TEST(LatencyHist, UsesBoundedStorage) {
  LatencyHist h;
  const size_t bins = h.storage_bins();
  for (uint64_t i = 0; i < 200000; ++i) h.record_us(i * 17);
  EXPECT_EQ(h.storage_bins(), bins);
  EXPECT_EQ(h.samples(), 200000u);
  EXPECT_GT(h.percentile_us(0.99), 0u);
}

TEST(LatencyHist, CoarseBinsReturnBoundedUpperValue) {
  LatencyHist h;
  for (uint64_t i = 0; i < 100; ++i) h.record_us(20000 + i);
  EXPECT_GE(h.percentile_us(0.95), 20094u);
  EXPECT_LE(h.percentile_us(0.95), 20100u);
}

TEST(DeliveryTracker, CountsAcceptedAndReceived) {
  DeliveryTracker d;
  d.mark_accepted(1, 0);
  d.mark_accepted(2, 0);
  d.mark_accepted(3, 0);
  d.mark_received(1, 0);
  d.mark_received(3, 0);
  d.mark_received(3, 0);
  EXPECT_EQ(d.accepted(), 3u);
  EXPECT_EQ(d.received(), 2u);
  EXPECT_EQ(d.dedup_entries(), 2u);
  EXPECT_DOUBLE_EQ(d.delivery_ratio(), 2.0 / 3.0);
}

TEST(DeliveryTracker, DedupWindowIsBoundedPerConnection) {
  DeliveryTracker d;
  for (size_t i = 0; i < DeliveryTracker::dedup_window_per_conn() + 10; ++i) {
    EXPECT_TRUE(d.mark_received(i, 0));
  }
  EXPECT_EQ(d.dedup_entries(), DeliveryTracker::dedup_window_per_conn());
  EXPECT_TRUE(d.mark_received(0, 0));
}

TEST(DeliveryTracker, DedupWindowIsPerConnection) {
  DeliveryTracker d;
  EXPECT_TRUE(d.mark_received(7, 0));
  EXPECT_TRUE(d.mark_received(7, 1));
  EXPECT_FALSE(d.mark_received(7, 0));
  EXPECT_EQ(d.received(), 2u);
  EXPECT_STREQ(d.dedup_policy(), "sliding_window_65536_per_conn");
}

TEST(SlidingDedupWindow, KeepsOnlyRecentKeys) {
  SlidingDedupWindow w(3);
  EXPECT_TRUE(w.insert(1));
  EXPECT_TRUE(w.insert(2));
  EXPECT_TRUE(w.insert(3));
  EXPECT_FALSE(w.insert(2));
  EXPECT_EQ(w.size(), 3u);

  EXPECT_TRUE(w.insert(4));
  EXPECT_EQ(w.size(), 3u);
  EXPECT_TRUE(w.insert(1));
  EXPECT_EQ(w.size(), 3u);
}

TEST(ThroughputCounter, BytesAndMessages) {
  ThroughputCounter t;
  t.record(64);
  t.record(64);
  t.record(64);
  EXPECT_EQ(t.bytes(), 192u);
  EXPECT_EQ(t.messages(), 3u);
}
