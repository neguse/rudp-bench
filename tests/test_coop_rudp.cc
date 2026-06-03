#include <gtest/gtest.h>

#include "coop_rudp/rudp.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

namespace {

struct Packet {
  rudp_addr from{};
  rudp_addr to{};
  std::vector<uint8_t> bytes;
};

struct FakeNet {
  std::deque<Packet> packets;
  uint64_t now_ns = 1'000'000'000ull;
  bool drop_next = false;
};

struct FakeSock {
  FakeNet* net = nullptr;
  rudp_addr self{};
};

rudp_addr addr(uint8_t v) {
  rudp_addr a{};
  a.len = 1;
  a.data[0] = v;
  return a;
}

bool same_addr(const rudp_addr& a, const rudp_addr& b) {
  return a.len == b.len && std::memcmp(a.data, b.data, a.len) == 0;
}

int send_batch(void* user, const rudp_out_packet* packets, size_t count) {
  auto* s = static_cast<FakeSock*>(user);
  if (s->net->drop_next) {
    s->net->drop_next = false;
    return static_cast<int>(count);
  }
  for (size_t i = 0; i < count; ++i) {
    Packet p;
    p.from = s->self;
    p.to = packets[i].addr;
    p.bytes.assign(packets[i].data, packets[i].data + packets[i].len);
    s->net->packets.push_back(std::move(p));
  }
  return static_cast<int>(count);
}

int recv_batch(void* user, rudp_in_packet* packets, size_t max_count) {
  auto* s = static_cast<FakeSock*>(user);
  size_t n = 0;
  for (auto it = s->net->packets.begin(); it != s->net->packets.end() && n < max_count;) {
    if (!same_addr(it->to, s->self)) {
      ++it;
      continue;
    }
    if (it->bytes.size() <= packets[n].cap) {
      packets[n].addr = it->from;
      packets[n].len = it->bytes.size();
      std::memcpy(packets[n].data, it->bytes.data(), it->bytes.size());
      ++n;
    }
    it = s->net->packets.erase(it);
  }
  return static_cast<int>(n);
}

uint64_t now_ns(void* user) {
  return static_cast<FakeSock*>(user)->net->now_ns;
}

rudp_endpoint_config config_for(FakeSock* sock, uint16_t mtu = 512,
                                uint16_t max_payload = 0) {
  rudp_endpoint_config cfg{};
  cfg.socket.user = sock;
  cfg.socket.send_batch = send_batch;
  cfg.socket.recv_batch = recv_batch;
  cfg.socket.now_ns = now_ns;
  cfg.mtu = mtu;
  cfg.max_payload_bytes = max_payload;
  cfg.max_conns = 8;
  cfg.max_flows = 8;
  cfg.max_channels = 8;
  cfg.max_messages = 128;
  cfg.max_recv_events = 64;
  cfg.max_ordered_holds = 32;
  cfg.sent_packet_count = 128;
  cfg.recv_batch_size = 16;
  cfg.send_batch_size = 16;
  cfg.rto_ms = 20;
  return cfg;
}

struct Pair {
  FakeNet net;
  FakeSock a{&net, addr(1)};
  FakeSock b{&net, addr(2)};
  rudp_endpoint* ea = nullptr;
  rudp_endpoint* eb = nullptr;
  rudp_conn* ca = nullptr;

  explicit Pair(uint16_t mtu = 512, uint16_t max_payload = 0) {
    auto ca_cfg = config_for(&a, mtu, max_payload);
    auto cb_cfg = config_for(&b, mtu, max_payload);
    EXPECT_EQ(rudp_endpoint_create(&ea, &ca_cfg), 0);
    EXPECT_EQ(rudp_endpoint_create(&eb, &cb_cfg), 0);
    auto baddr = addr(2);
    EXPECT_EQ(rudp_endpoint_connect(ea, &baddr, 42, &ca), 0);
  }

  ~Pair() {
    rudp_endpoint_destroy(ea);
    rudp_endpoint_destroy(eb);
  }

  void pump() {
    rudp_endpoint_flush(ea, net.now_ns);
    rudp_endpoint_poll(eb, net.now_ns);
    rudp_endpoint_flush(eb, net.now_ns);
    rudp_endpoint_poll(ea, net.now_ns);
  }
};

}  // namespace

TEST(CoopRudp, SendQueuesUntilFlush) {
  Pair p;
  const char msg[] = "hello";
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE;
  EXPECT_EQ(rudp_send(p.ca, msg, sizeof(msg), &opts), RUDP_SEND_QUEUED);
  EXPECT_TRUE(p.net.packets.empty());

  rudp_endpoint_flush(p.ea, p.net.now_ns);
  EXPECT_FALSE(p.net.packets.empty());
}

TEST(CoopRudp, FlushCoalescesMultipleFramesForSameConnection) {
  Pair p;
  const char a[] = "a";
  const char b[] = "b";
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE;
  ASSERT_EQ(rudp_send(p.ca, a, sizeof(a), &opts), RUDP_SEND_QUEUED);
  ASSERT_EQ(rudp_send(p.ca, b, sizeof(b), &opts), RUDP_SEND_QUEUED);

  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_EQ(p.net.packets.size(), 1u);
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  char out[32]{};
  size_t len = 0;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_STREQ(out, a);
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_STREQ(out, b);
}

TEST(CoopRudp, UnreliableMessageDeliversWithMetadata) {
  Pair p;
  const char msg[] = "pose";
  rudp_send_opts opts{};
  opts.flow_id = 3;
  opts.channel_id = 2;
  opts.reliability = RUDP_UNRELIABLE;
  ASSERT_EQ(rudp_send(p.ca, msg, sizeof(msg), &opts), RUDP_SEND_QUEUED);

  p.pump();
  char out[32]{};
  size_t len = 0;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_EQ(len, sizeof(msg));
  EXPECT_STREQ(out, msg);
  EXPECT_EQ(info.conn_id, 42u);
  EXPECT_EQ(info.flow_id, 3u);
  EXPECT_EQ(info.channel_id, 2u);
  EXPECT_EQ(info.reliability, RUDP_UNRELIABLE);
}

TEST(CoopRudp, BorrowMetaReturnsConnIdAndReliability) {
  Pair p;
  const char msg[] = "meta";
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE;
  ASSERT_EQ(rudp_send(p.ca, msg, sizeof(msg), &opts), RUDP_SEND_QUEUED);

  p.pump();
  const void* data = nullptr;
  size_t len = 0;
  uint64_t conn_id = 0;
  rudp_reliability reliability = RUDP_RELIABLE_ORDERED;
  ASSERT_EQ(rudp_recv_borrow_meta(p.eb, &data, &len, &conn_id, &reliability), 1);
  ASSERT_NE(data, nullptr);
  EXPECT_EQ(len, sizeof(msg));
  EXPECT_EQ(std::memcmp(data, msg, sizeof(msg)), 0);
  EXPECT_EQ(conn_id, 42u);
  EXPECT_EQ(reliability, RUDP_UNRELIABLE);
}

TEST(CoopRudp, FlowLimitRejectsZeroBpsFlow) {
  Pair p;
  rudp_flow_limit lim{};
  lim.flow_id = 1;
  lim.max_bps = 0;
  lim.max_queue_bytes = 1024;
  rudp_set_flow_limits(p.ca, &lim, 1);

  const char msg[] = "drop";
  rudp_send_opts opts{};
  opts.flow_id = 1;
  opts.reliability = RUDP_UNRELIABLE;
  EXPECT_EQ(rudp_send(p.ca, msg, sizeof(msg), &opts), RUDP_SEND_RATE_LIMITED);
}

TEST(CoopRudp, FlowMaxDelayExpiresQueuedMessageBeforeFlush) {
  Pair p;
  rudp_flow_limit lim{};
  lim.flow_id = 2;
  lim.max_bps = UINT32_MAX;
  lim.max_queue_bytes = 1024;
  lim.max_delay_ms = 5;
  rudp_set_flow_limits(p.ca, &lim, 1);

  const char msg[] = "too-late";
  rudp_send_opts opts{};
  opts.flow_id = 2;
  opts.reliability = RUDP_UNRELIABLE;
  ASSERT_EQ(rudp_send(p.ca, msg, sizeof(msg), &opts), RUDP_SEND_QUEUED);
  p.net.now_ns += 6'000'000ull;
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  EXPECT_TRUE(p.net.packets.empty());

  rudp_flow_stats stats[8]{};
  ASSERT_GE(rudp_get_flow_stats(p.ca, stats, 8), 3u);
  EXPECT_EQ(stats[2].expired, 1u);
}

TEST(CoopRudp, ReliableRetransmitsAfterLostPacket) {
  Pair p;
  const char msg[] = "event";
  rudp_send_opts opts{};
  opts.reliability = RUDP_RELIABLE_UNORDERED;
  ASSERT_EQ(rudp_send(p.ca, msg, sizeof(msg), &opts), RUDP_SEND_QUEUED);

  p.net.drop_next = true;
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  rudp_endpoint_poll(p.eb, p.net.now_ns);
  char out[32]{};
  size_t len = 0;
  rudp_recv_info info{};
  EXPECT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 0);

  p.net.now_ns += 25'000'000ull;
  p.pump();
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_STREQ(out, msg);
  EXPECT_EQ(info.reliability, RUDP_RELIABLE_UNORDERED);
}

TEST(CoopRudp, FragmentedReliableReassemblesBeforeDelivery) {
  Pair p(/*mtu=*/96, /*max_payload=*/160);
  std::array<uint8_t, 130> msg{};
  for (size_t i = 0; i < msg.size(); ++i) msg[i] = static_cast<uint8_t>(i + 1);
  rudp_send_opts opts{};
  opts.reliability = RUDP_RELIABLE_UNORDERED;
  ASSERT_EQ(rudp_send(p.ca, msg.data(), msg.size(), &opts), RUDP_SEND_QUEUED);

  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_EQ(p.net.packets.size(), 3u);

  Packet held = std::move(p.net.packets.back());
  p.net.packets.pop_back();
  rudp_endpoint_poll(p.eb, p.net.now_ns);
  std::array<uint8_t, 160> out{};
  size_t len = 0;
  rudp_recv_info info{};
  EXPECT_EQ(rudp_recv(p.eb, out.data(), out.size(), &len, &info), 0);

  p.net.packets.push_back(std::move(held));
  rudp_endpoint_poll(p.eb, p.net.now_ns);
  ASSERT_EQ(rudp_recv(p.eb, out.data(), out.size(), &len, &info), 1);
  EXPECT_EQ(len, msg.size());
  EXPECT_EQ(std::memcmp(out.data(), msg.data(), msg.size()), 0);
  EXPECT_EQ(info.reliability, RUDP_RELIABLE_UNORDERED);
}

TEST(CoopRudp, SequencedDropsOlderMessages) {
  Pair p;
  const char old_msg[] = "old";
  const char new_msg[] = "new";
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE_SEQUENCED;
  ASSERT_EQ(rudp_send(p.ca, old_msg, sizeof(old_msg), &opts), RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_EQ(rudp_send(p.ca, new_msg, sizeof(new_msg), &opts), RUDP_SEND_QUEUED);

  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_GE(p.net.packets.size(), 2u);
  std::swap(p.net.packets[0], p.net.packets[1]);
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  char out[32]{};
  size_t len = 0;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_STREQ(out, new_msg);
  EXPECT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 0);
}

TEST(CoopRudp, ReliableOrderedHoldsUntilMissingMessageArrives) {
  Pair p;
  const char first[] = "first";
  const char second[] = "second";
  rudp_send_opts opts{};
  opts.reliability = RUDP_RELIABLE_ORDERED;
  ASSERT_EQ(rudp_send(p.ca, first, sizeof(first), &opts), RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_EQ(rudp_send(p.ca, second, sizeof(second), &opts), RUDP_SEND_QUEUED);

  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_GE(p.net.packets.size(), 2u);
  std::swap(p.net.packets[0], p.net.packets[1]);
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  char out[32]{};
  size_t len = 0;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_STREQ(out, first);
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_STREQ(out, second);
}
