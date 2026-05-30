#include <gtest/gtest.h>
#include "harness/inbound_queue.h"

#include <cstring>

using namespace rudp_bench;

TEST(ReusableInboundQueue, PreservesMessageOrderAndConnId) {
  ReusableInboundQueue q;
  const char a[] = "first";
  const char b[] = "second";
  q.enqueue(7, reinterpret_cast<const uint8_t*>(a), sizeof(a));
  q.enqueue(9, reinterpret_cast<const uint8_t*>(b), sizeof(b));

  char buf[32];
  size_t len = 0;
  uint32_t cid = 0;
  EXPECT_EQ(q.recv(buf, sizeof(buf), &len, &cid), 1);
  EXPECT_EQ(cid, 7u);
  EXPECT_EQ(len, sizeof(a));
  EXPECT_EQ(std::memcmp(buf, a, sizeof(a)), 0);

  EXPECT_EQ(q.recv(buf, sizeof(buf), &len, &cid), 1);
  EXPECT_EQ(cid, 9u);
  EXPECT_EQ(len, sizeof(b));
  EXPECT_EQ(std::memcmp(buf, b, sizeof(b)), 0);
  EXPECT_EQ(q.recv(buf, sizeof(buf), &len, &cid), 0);
}

TEST(ReusableInboundQueue, OversizeDropsMessageAndRecyclesBuffer) {
  ReusableInboundQueue q;
  const char msg[] = "too-large";
  q.enqueue(3, reinterpret_cast<const uint8_t*>(msg), sizeof(msg));

  char buf[2];
  size_t len = 0;
  uint32_t cid = 0;
  EXPECT_EQ(q.recv(buf, sizeof(buf), &len, &cid), -1);
  EXPECT_EQ(cid, 3u);
  EXPECT_EQ(len, sizeof(msg));
  EXPECT_EQ(q.queued(), 0u);
  EXPECT_EQ(q.free_buffers(), 1u);
}

TEST(ReusableInboundQueue, BoundedRingDropsOldestAndCounts) {
  ReusableInboundQueue q;
  q.set_limit(2);
  const char m[] = "m";
  q.enqueue(1, reinterpret_cast<const uint8_t*>(m), sizeof(m));
  q.enqueue(2, reinterpret_cast<const uint8_t*>(m), sizeof(m));
  q.enqueue(3, reinterpret_cast<const uint8_t*>(m), sizeof(m));  // evicts cid=1
  EXPECT_EQ(q.queued(), 2u);
  EXPECT_EQ(q.dropped(), 1u);

  char buf[8];
  size_t len = 0;
  uint32_t cid = 0;
  EXPECT_EQ(q.recv(buf, sizeof(buf), &len, &cid), 1);
  EXPECT_EQ(cid, 2u);  // oldest (cid=1) was dropped, cid=2 is now front
  EXPECT_EQ(q.recv(buf, sizeof(buf), &len, &cid), 1);
  EXPECT_EQ(cid, 3u);
  EXPECT_EQ(q.recv(buf, sizeof(buf), &len, &cid), 0);
}

TEST(ReusableInboundQueue, ReusesFreedBuffers) {
  ReusableInboundQueue q;
  const char msg[] = "reused";
  char buf[32];
  size_t len = 0;
  uint32_t cid = 0;

  q.enqueue(1, reinterpret_cast<const uint8_t*>(msg), sizeof(msg));
  EXPECT_EQ(q.recv(buf, sizeof(buf), &len, &cid), 1);
  EXPECT_EQ(q.free_buffers(), 1u);

  q.enqueue(1, reinterpret_cast<const uint8_t*>(msg), sizeof(msg));
  EXPECT_EQ(q.free_buffers(), 0u);
  EXPECT_EQ(q.recv(buf, sizeof(buf), &len, &cid), 1);
}
