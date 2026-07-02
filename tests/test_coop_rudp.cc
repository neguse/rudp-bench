#include <gtest/gtest.h>

#include "coop_rudp/rudp.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

namespace {

constexpr uint32_t kPacketMagic = 0x52554400u;
constexpr uint32_t kPacketHeaderBytes = 32u;
constexpr uint32_t kDataFrameHeaderBytes = 20u;
constexpr uint8_t kAckOnlyFlag = 0x01u;
constexpr uint8_t kFrameData = 1u;
constexpr uint8_t kReliableFlag = 0x01u;
constexpr uint8_t kOrderedFlag = 0x02u;
constexpr uint8_t kSequencedFlag = 0x04u;

struct Packet {
  rudp_addr from{};
  rudp_addr to{};
  std::vector<uint8_t> bytes;
};

struct FakeNet {
  std::deque<Packet> packets;
  uint64_t now_ns = 1'000'000'000ull;
  bool drop_next = false;
  bool drop_first_packet_once = false;
  bool fail_recv_and_poison_slot_once = false;
  int send_limit_once = -1;
  size_t recv_cap_override_once = 0;
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

uint32_t load_u32_le(const std::vector<uint8_t>& bytes, size_t off) {
  return static_cast<uint32_t>(bytes[off]) |
         (static_cast<uint32_t>(bytes[off + 1]) << 8) |
         (static_cast<uint32_t>(bytes[off + 2]) << 16) |
         (static_cast<uint32_t>(bytes[off + 3]) << 24);
}

uint64_t load_u64_le(const std::vector<uint8_t>& bytes, size_t off) {
  uint64_t v = 0;
  for (size_t i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(bytes[off + i]) << (i * 8);
  }
  return v;
}

void store_u16_le(std::vector<uint8_t>& bytes, size_t off, uint16_t v) {
  bytes[off + 0] = static_cast<uint8_t>(v & 0xffu);
  bytes[off + 1] = static_cast<uint8_t>((v >> 8) & 0xffu);
}

void store_u32_le(std::vector<uint8_t>& bytes, size_t off, uint32_t v) {
  bytes[off + 0] = static_cast<uint8_t>(v & 0xffu);
  bytes[off + 1] = static_cast<uint8_t>((v >> 8) & 0xffu);
  bytes[off + 2] = static_cast<uint8_t>((v >> 16) & 0xffu);
  bytes[off + 3] = static_cast<uint8_t>((v >> 24) & 0xffu);
}

void store_u64_le(std::vector<uint8_t>& bytes, size_t off, uint64_t v) {
  for (size_t i = 0; i < 8; ++i) {
    bytes[off + i] = static_cast<uint8_t>((v >> (i * 8)) & 0xffu);
  }
}

std::vector<uint8_t> make_single_frame_packet(uint8_t flags,
                                              uint16_t payload_len,
                                              uint16_t channel_seq,
                                              size_t trailing_bytes = 0) {
  std::vector<uint8_t> bytes(kPacketHeaderBytes + kDataFrameHeaderBytes +
                             payload_len + trailing_bytes);
  store_u32_le(bytes, 0, kPacketMagic);
  store_u64_le(bytes, 4, 42);
  store_u32_le(bytes, 12, 7);
  store_u32_le(bytes, 16, 0);
  store_u64_le(bytes, 20, 0);
  store_u16_le(bytes, 28, kPacketHeaderBytes);
  store_u16_le(bytes, 30,
               static_cast<uint16_t>(kDataFrameHeaderBytes + payload_len +
                                     trailing_bytes));

  size_t off = kPacketHeaderBytes;
  bytes[off + 0] = kFrameData;
  bytes[off + 1] = flags;
  store_u16_le(bytes, off + 2, kDataFrameHeaderBytes);
  store_u16_le(bytes, off + 4, 0);
  store_u16_le(bytes, off + 6, payload_len);
  store_u32_le(bytes, off + 8, 123);
  store_u16_le(bytes, off + 12, 0);
  store_u16_le(bytes, off + 14, 1);
  store_u16_le(bytes, off + 16, 0);
  store_u16_le(bytes, off + 18, channel_seq);
  std::fill(bytes.begin() + off + kDataFrameHeaderBytes,
            bytes.begin() + off + kDataFrameHeaderBytes + payload_len, 0xcc);
  if (trailing_bytes > 0) {
    auto trailing =
        static_cast<std::vector<uint8_t>::difference_type>(trailing_bytes);
    std::fill(bytes.end() - trailing, bytes.end(), 0xee);
  }
  return bytes;
}

std::vector<uint8_t> make_ack_only_packet(uint64_t conn_id, uint32_t ack_seq) {
  std::vector<uint8_t> bytes(kPacketHeaderBytes);
  store_u32_le(bytes, 0, kPacketMagic | kAckOnlyFlag);
  store_u64_le(bytes, 4, conn_id);
  store_u32_le(bytes, 12, 0);
  store_u32_le(bytes, 16, ack_seq);
  store_u64_le(bytes, 20, 0);
  store_u16_le(bytes, 28, kPacketHeaderBytes);
  store_u16_le(bytes, 30, 0);
  return bytes;
}

std::vector<uint8_t> make_empty_data_packet(uint64_t conn_id,
                                            uint32_t packet_seq) {
  std::vector<uint8_t> bytes(kPacketHeaderBytes);
  store_u32_le(bytes, 0, kPacketMagic);
  store_u64_le(bytes, 4, conn_id);
  store_u32_le(bytes, 12, packet_seq);
  store_u32_le(bytes, 16, 0);
  store_u64_le(bytes, 20, 0);
  store_u16_le(bytes, 28, kPacketHeaderBytes);
  store_u16_le(bytes, 30, 0);
  return bytes;
}

std::vector<uint8_t> make_fragment_packet(uint8_t flags, uint32_t packet_seq,
                                          uint32_t msg_id,
                                          uint16_t frag_index,
                                          uint16_t frag_count,
                                          uint16_t payload_len,
                                          uint8_t fill) {
  std::vector<uint8_t> bytes =
      make_single_frame_packet(flags, payload_len, /*channel_seq=*/0);
  store_u32_le(bytes, 12, packet_seq);
  size_t off = kPacketHeaderBytes;
  store_u32_le(bytes, off + 8, msg_id);
  store_u16_le(bytes, off + 12, frag_index);
  store_u16_le(bytes, off + 14, frag_count);
  std::fill(bytes.begin() + off + kDataFrameHeaderBytes, bytes.end(), fill);
  return bytes;
}

std::vector<uint8_t> make_two_frame_packet(uint16_t payload_len) {
  std::vector<uint8_t> bytes(kPacketHeaderBytes +
                             2u * (kDataFrameHeaderBytes + payload_len));
  store_u32_le(bytes, 0, kPacketMagic);
  store_u64_le(bytes, 4, 42);
  store_u32_le(bytes, 12, 7);
  store_u32_le(bytes, 16, 0);
  store_u64_le(bytes, 20, 0);
  store_u16_le(bytes, 28, kPacketHeaderBytes);
  store_u16_le(bytes, 30,
               static_cast<uint16_t>(2u * (kDataFrameHeaderBytes + payload_len)));

  for (uint16_t frame = 0; frame < 2; ++frame) {
    size_t off = kPacketHeaderBytes +
                 static_cast<size_t>(frame) * (kDataFrameHeaderBytes + payload_len);
    bytes[off + 0] = kFrameData;
    bytes[off + 1] = 0;
    store_u16_le(bytes, off + 2, kDataFrameHeaderBytes);
    store_u16_le(bytes, off + 4, 0);
    store_u16_le(bytes, off + 6, payload_len);
    store_u32_le(bytes, off + 8, 123u + frame);
    store_u16_le(bytes, off + 12, 0);
    store_u16_le(bytes, off + 14, 1);
    store_u16_le(bytes, off + 16, 0);
    store_u16_le(bytes, off + 18, 0);
    std::fill(bytes.begin() + off + kDataFrameHeaderBytes,
              bytes.begin() + off + kDataFrameHeaderBytes + payload_len,
              static_cast<uint8_t>('a' + frame));
  }
  return bytes;
}

int send_batch(void* user, const rudp_out_packet* packets, size_t count) {
  auto* s = static_cast<FakeSock*>(user);
  if (s->net->drop_next) {
    s->net->drop_next = false;
    return static_cast<int>(count);
  }
  size_t accepted = count;
  if (s->net->send_limit_once >= 0) {
    accepted = std::min(count, static_cast<size_t>(s->net->send_limit_once));
    s->net->send_limit_once = -1;
  }
  for (size_t i = 0; i < accepted; ++i) {
    if (s->net->drop_first_packet_once) {
      s->net->drop_first_packet_once = false;
      continue;
    }
    Packet p;
    p.from = s->self;
    p.to = packets[i].addr;
    p.bytes.assign(packets[i].data, packets[i].data + packets[i].len);
    s->net->packets.push_back(std::move(p));
  }
  return static_cast<int>(accepted);
}

int recv_batch(void* user, rudp_in_packet* packets, size_t max_count) {
  auto* s = static_cast<FakeSock*>(user);
  if (s->net->fail_recv_and_poison_slot_once) {
    s->net->fail_recv_and_poison_slot_once = false;
    if (max_count > 0) {
      packets[0].data = nullptr;
      packets[0].cap = 0;
      packets[0].len = 0;
    }
    return -1;
  }
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
      if (s->net->recv_cap_override_once > 0) {
        packets[n].cap = s->net->recv_cap_override_once;
        s->net->recv_cap_override_once = 0;
      }
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
                                uint16_t max_payload = 0,
                                uint32_t send_batch_size = 16,
                                uint32_t max_recv_events = 64,
                                uint32_t max_ordered_holds = 32,
                                uint32_t sent_packet_count = 128) {
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
  cfg.max_recv_events = max_recv_events;
  cfg.max_ordered_holds = max_ordered_holds;
  cfg.sent_packet_count = sent_packet_count;
  cfg.recv_batch_size = 16;
  cfg.send_batch_size = send_batch_size;
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

  explicit Pair(uint16_t mtu = 512, uint16_t max_payload = 0,
                uint32_t send_batch_size = 16,
                uint32_t max_recv_events = 64,
                uint32_t max_ordered_holds = 32,
                uint32_t sent_packet_count = 128,
                uint32_t max_retransmits = 0,
                uint32_t idle_timeout_ms = 0) {
    auto ca_cfg = config_for(&a, mtu, max_payload, send_batch_size,
                             max_recv_events, max_ordered_holds,
                             sent_packet_count);
    auto cb_cfg = config_for(&b, mtu, max_payload, send_batch_size,
                             max_recv_events, max_ordered_holds,
                             sent_packet_count);
    ca_cfg.max_retransmits = max_retransmits;
    ca_cfg.idle_timeout_ms = idle_timeout_ms;
    cb_cfg.max_retransmits = max_retransmits;
    cb_cfg.idle_timeout_ms = idle_timeout_ms;
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

TEST(CoopRudp, ConnectRejectsInvalidPeerAddressLength) {
  FakeNet net;
  FakeSock sock{&net, addr(1)};
  rudp_endpoint_config cfg = config_for(&sock);
  rudp_endpoint* ep = nullptr;
  ASSERT_EQ(rudp_endpoint_create(&ep, &cfg), 0);

  rudp_addr bad = addr(2);
  bad.len = sizeof(bad.data) + 1;
  rudp_conn* conn = reinterpret_cast<rudp_conn*>(1);
  EXPECT_EQ(rudp_endpoint_connect(ep, &bad, 42, &conn), -1);
  EXPECT_EQ(conn, nullptr);

  rudp_addr empty{};
  conn = reinterpret_cast<rudp_conn*>(1);
  EXPECT_EQ(rudp_endpoint_connect(ep, &empty, 42, &conn), -1);
  EXPECT_EQ(conn, nullptr);
  rudp_endpoint_destroy(ep);
}

TEST(CoopRudp, IncomingPacketWithEmptyAddressDoesNotCreateConnection) {
  Pair p;
  Packet bad;
  bad.from = rudp_addr{};
  bad.to = addr(2);
  bad.bytes = make_single_frame_packet(kReliableFlag, /*payload_len=*/1,
                                       /*channel_seq=*/0);
  p.net.packets.push_back(std::move(bad));

  rudp_endpoint_poll(p.eb, p.net.now_ns);
  EXPECT_EQ(rudp_endpoint_find_conn(p.eb, 42), nullptr);

  char out[8]{};
  size_t len = 0;
  rudp_recv_info info{};
  EXPECT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 0);
}

TEST(CoopRudp, EndpointCreateRejectsOverflowingConnectionMapConfig) {
  FakeNet net;
  FakeSock sock{&net, addr(1)};
  rudp_endpoint_config cfg = config_for(&sock);
  cfg.max_conns = 0x20000001u;

  rudp_endpoint* ep = nullptr;
  EXPECT_EQ(rudp_endpoint_create(&ep, &cfg), -1);
  EXPECT_EQ(ep, nullptr);
}

TEST(CoopRudp, EndpointCreateRejectsOverflowingQueueStrideConfig) {
  FakeNet net;
  FakeSock sock{&net, addr(1)};
  rudp_endpoint_config cfg = config_for(&sock);
  cfg.max_conns = 1;
  cfg.max_flows = 1;
  cfg.max_channels = 1;
  cfg.max_messages = UINT32_MAX;

  rudp_endpoint* ep = nullptr;
  EXPECT_EQ(rudp_endpoint_create(&ep, &cfg), -1);
  EXPECT_EQ(ep, nullptr);
}

TEST(CoopRudp, EndpointCreateRejectsUnaddressableFlowAndChannelCounts) {
  FakeNet net;
  FakeSock sock{&net, addr(1)};
  rudp_endpoint_config cfg = config_for(&sock);
  cfg.max_conns = 1;
  cfg.max_flows = 65537;
  cfg.max_channels = 1;

  rudp_endpoint* ep = nullptr;
  EXPECT_EQ(rudp_endpoint_create(&ep, &cfg), -1);
  EXPECT_EQ(ep, nullptr);

  cfg.max_flows = 1;
  cfg.max_channels = 65537;
  EXPECT_EQ(rudp_endpoint_create(&ep, &cfg), -1);
  EXPECT_EQ(ep, nullptr);
}

TEST(CoopRudp, EndpointCreateRejectsUnfragmentableMaxPayload) {
  FakeNet net;
  FakeSock sock{&net, addr(1)};
  rudp_endpoint_config cfg = config_for(&sock);
  cfg.mtu = kPacketHeaderBytes + kDataFrameHeaderBytes + 1;
  cfg.max_payload_bytes = 257;

  rudp_endpoint* ep = nullptr;
  EXPECT_EQ(rudp_endpoint_create(&ep, &cfg), -1);
  EXPECT_EQ(ep, nullptr);
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

TEST(CoopRudp, ZeroLengthSendAllowsNullData) {
  Pair p;
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE;
  ASSERT_EQ(rudp_send(p.ca, nullptr, 0, &opts), RUDP_SEND_QUEUED);

  p.pump();
  std::array<uint8_t, 1> out{};
  size_t len = 1;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv(p.eb, out.data(), out.size(), &len, &info), 1);
  EXPECT_EQ(len, 0u);
}

TEST(CoopRudp, ZeroLengthRecvAllowsNullBuffer) {
  Pair p;
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE;
  ASSERT_EQ(rudp_send(p.ca, nullptr, 0, &opts), RUDP_SEND_QUEUED);

  p.pump();
  size_t len = 1;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv(p.eb, nullptr, 0, &len, &info), 1);
  EXPECT_EQ(len, 0u);
}

TEST(CoopRudp, SendQueueAllowsExactRingCapacity) {
  FakeNet net;
  FakeSock sock{&net, addr(1)};
  rudp_endpoint_config cfg = config_for(&sock);
  cfg.max_conns = 1;
  cfg.max_flows = 1;
  cfg.max_channels = 1;
  cfg.max_messages = 1024;
  rudp_endpoint* ep = nullptr;
  ASSERT_EQ(rudp_endpoint_create(&ep, &cfg), 0);
  rudp_conn* conn = nullptr;
  rudp_addr peer = addr(2);
  ASSERT_EQ(rudp_endpoint_connect(ep, &peer, 42, &conn), 0);

  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE;
  for (int i = 0; i < 1024; ++i) {
    ASSERT_EQ(rudp_send(conn, nullptr, 0, &opts), RUDP_SEND_QUEUED) << i;
  }
  EXPECT_EQ(rudp_send(conn, nullptr, 0, &opts), RUDP_SEND_QUEUE_FULL);
  rudp_endpoint_destroy(ep);
}

TEST(CoopRudp, PartialSendKeepsUnsentMessagesQueued) {
  Pair p(/*mtu=*/64);
  std::array<uint8_t, 12> a{};
  std::array<uint8_t, 12> b{};
  a.fill(0xA1);
  b.fill(0xB2);
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE;
  ASSERT_EQ(rudp_send(p.ca, a.data(), a.size(), &opts), RUDP_SEND_QUEUED);
  ASSERT_EQ(rudp_send(p.ca, b.data(), b.size(), &opts), RUDP_SEND_QUEUED);

  p.net.send_limit_once = 1;
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_EQ(p.net.packets.size(), 1u);
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  std::array<uint8_t, 12> out{};
  size_t len = 0;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv(p.eb, out.data(), out.size(), &len, &info), 1);
  EXPECT_EQ(len, a.size());
  EXPECT_EQ(out, a);

  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_EQ(p.net.packets.size(), 1u);
  rudp_endpoint_poll(p.eb, p.net.now_ns);
  ASSERT_EQ(rudp_recv(p.eb, out.data(), out.size(), &len, &info), 1);
  EXPECT_EQ(len, b.size());
  EXPECT_EQ(out, b);
}

TEST(CoopRudp, PartialMidFlushPreservesQueueOrder) {
  Pair p(/*mtu=*/64, /*max_payload=*/0, /*send_batch_size=*/1);
  std::array<uint8_t, 12> a{};
  std::array<uint8_t, 12> b{};
  a.fill(0xA3);
  b.fill(0xB4);
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE;
  ASSERT_EQ(rudp_send(p.ca, a.data(), a.size(), &opts), RUDP_SEND_QUEUED);
  ASSERT_EQ(rudp_send(p.ca, b.data(), b.size(), &opts), RUDP_SEND_QUEUED);

  p.net.send_limit_once = 0;
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  EXPECT_TRUE(p.net.packets.empty());
  rudp_flow_stats stats[8]{};
  ASSERT_GE(rudp_get_flow_stats(p.ca, stats, 8), 1u);
  EXPECT_EQ(stats[0].sent_bps, 0u);

  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_EQ(p.net.packets.size(), 2u);
  ASSERT_GE(rudp_get_flow_stats(p.ca, stats, 8), 1u);
  EXPECT_EQ(stats[0].sent_bps, a.size() + b.size());
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  std::array<uint8_t, 12> out{};
  size_t len = 0;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv(p.eb, out.data(), out.size(), &len, &info), 1);
  EXPECT_EQ(out, a);
  ASSERT_EQ(rudp_recv(p.eb, out.data(), out.size(), &len, &info), 1);
  EXPECT_EQ(out, b);
}

TEST(CoopRudp, SendPriorityOrdersQueuedMessagesWithinFlow) {
  Pair p;
  const char low[] = "low-priority";
  const char high[] = "high-priority";
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE;
  opts.priority = 0;
  ASSERT_EQ(rudp_send(p.ca, low, sizeof(low), &opts), RUDP_SEND_QUEUED);
  opts.priority = 10;
  ASSERT_EQ(rudp_send(p.ca, high, sizeof(high), &opts), RUDP_SEND_QUEUED);

  p.pump();
  char out[32]{};
  size_t len = 0;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_STREQ(out, high);
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_STREQ(out, low);
}

TEST(CoopRudp, FiniteFlowLimitClampsPreviouslyUnlimitedTokens) {
  Pair p(/*mtu=*/1200, /*max_payload=*/1000);
  std::array<uint8_t, 16> warmup{};
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE;
  ASSERT_EQ(rudp_send(p.ca, warmup.data(), warmup.size(), &opts), RUDP_SEND_QUEUED);
  p.pump();

  rudp_flow_limit lim{};
  lim.flow_id = 0;
  lim.max_bps = 8;
  lim.max_queue_bytes = UINT32_MAX;
  rudp_set_flow_limits(p.ca, &lim, 1);

  std::array<uint8_t, 1000> a{};
  std::array<uint8_t, 1000> b{};
  a.fill(0xA1);
  b.fill(0xB2);
  ASSERT_EQ(rudp_send(p.ca, a.data(), a.size(), &opts), RUDP_SEND_QUEUED);
  ASSERT_EQ(rudp_send(p.ca, b.data(), b.size(), &opts), RUDP_SEND_QUEUED);

  rudp_endpoint_flush(p.ea, p.net.now_ns);
  EXPECT_EQ(p.net.packets.size(), 1u);

  rudp_status st{};
  rudp_get_status(p.ca, &st);
  EXPECT_EQ(st.send_queue_bytes, b.size());
}

TEST(CoopRudp, LowRateFlowCanSendOneLargeMtuFrame) {
  Pair p(/*mtu=*/4096, /*max_payload=*/4000);
  rudp_flow_limit lim{};
  lim.flow_id = 0;
  lim.max_bps = 8;
  lim.max_queue_bytes = UINT32_MAX;
  rudp_set_flow_limits(p.ca, &lim, 1);

  std::array<uint8_t, 3000> msg{};
  msg.fill(0x5a);
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE;
  ASSERT_EQ(rudp_send(p.ca, msg.data(), msg.size(), &opts), RUDP_SEND_QUEUED);

  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_EQ(p.net.packets.size(), 1u);
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  std::array<uint8_t, 3000> out{};
  size_t len = 0;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv(p.eb, out.data(), out.size(), &len, &info), 1);
  EXPECT_EQ(len, msg.size());
  EXPECT_EQ(out, msg);
}

TEST(CoopRudp, LongElapsedFlowRefillDoesNotOverflow) {
  Pair p(/*mtu=*/1200, /*max_payload=*/1000);
  p.net.now_ns = 1;
  rudp_flow_limit lim{};
  lim.flow_id = 0;
  lim.max_bps = 8;
  lim.max_queue_bytes = UINT32_MAX;
  rudp_set_flow_limits(p.ca, &lim, 1);

  std::array<uint8_t, 1000> first{};
  std::array<uint8_t, 1000> second{};
  first.fill(0x11);
  second.fill(0x22);
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE;
  ASSERT_EQ(rudp_send(p.ca, first.data(), first.size(), &opts), RUDP_SEND_QUEUED);
  ASSERT_EQ(rudp_send(p.ca, second.data(), second.size(), &opts), RUDP_SEND_QUEUED);

  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_EQ(p.net.packets.size(), 1u);
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  p.net.now_ns = UINT64_MAX / 8ull + 2ull;
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_EQ(p.net.packets.size(), 1u);
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  std::array<uint8_t, 1000> out{};
  size_t len = 0;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv(p.eb, out.data(), out.size(), &len, &info), 1);
  EXPECT_EQ(out, first);
  ASSERT_EQ(rudp_recv(p.eb, out.data(), out.size(), &len, &info), 1);
  EXPECT_EQ(out, second);
}

TEST(CoopRudp, QueuedRecvSurvivesLaterPoll) {
  Pair p;
  const char first[] = "first-event";
  const char second[] = "second-event";
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE;
  ASSERT_EQ(rudp_send(p.ca, first, sizeof(first), &opts), RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  ASSERT_EQ(rudp_send(p.ca, second, sizeof(second), &opts), RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  char out[32]{};
  size_t len = 0;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_STREQ(out, first);
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_STREQ(out, second);
}

TEST(CoopRudp, RecvBatchErrorRestoresInternalSlotsForNextPoll) {
  Pair p;
  const char msg[] = "after-error";
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE;
  ASSERT_EQ(rudp_send(p.ca, msg, sizeof(msg), &opts), RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_EQ(p.net.packets.size(), 1u);

  p.net.fail_recv_and_poison_slot_once = true;
  rudp_endpoint_poll(p.eb, p.net.now_ns);
  ASSERT_EQ(p.net.packets.size(), 1u);

  rudp_endpoint_poll(p.eb, p.net.now_ns);
  char out[32]{};
  size_t len = 0;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_STREQ(out, msg);
}

TEST(CoopRudp, RecvTooSmallReportsNeededLengthAndKeepsEvent) {
  Pair p;
  const char msg[] = "needs-a-larger-buffer";
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE;
  ASSERT_EQ(rudp_send(p.ca, msg, sizeof(msg), &opts), RUDP_SEND_QUEUED);
  p.pump();

  char small[4]{};
  size_t len = 0;
  rudp_recv_info info{};
  EXPECT_EQ(rudp_recv(p.eb, small, sizeof(small), &len, &info), -1);
  EXPECT_EQ(len, sizeof(msg));
  EXPECT_EQ(info.conn_id, 42u);
  EXPECT_EQ(info.reliability, RUDP_UNRELIABLE);

  char out[64]{};
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_STREQ(out, msg);
  EXPECT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 0);
}

TEST(CoopRudp, RecvInvalidArgsReturnErrorWithoutConsumingEvent) {
  Pair p;
  const char msg[] = "valid-after-invalid";
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE;
  ASSERT_EQ(rudp_send(p.ca, msg, sizeof(msg), &opts), RUDP_SEND_QUEUED);
  p.pump();

  char out[64]{};
  size_t len = 0;
  rudp_recv_info info{};
  EXPECT_EQ(rudp_recv(nullptr, out, sizeof(out), &len, &info), -1);
  EXPECT_EQ(rudp_recv(p.eb, out, sizeof(out), nullptr, &info), -1);
  EXPECT_EQ(rudp_recv(p.eb, nullptr, sizeof(out), &len, &info), -1);

  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_EQ(len, sizeof(msg));
  EXPECT_STREQ(out, msg);
}

TEST(CoopRudp, InvalidReliabilityIsRejected) {
  Pair p;
  const char msg[] = "invalid";
  rudp_send_opts opts{};
  opts.reliability = static_cast<rudp_reliability>(99);
  EXPECT_EQ(rudp_send(p.ca, msg, sizeof(msg), &opts), RUDP_SEND_UNUSABLE);

  rudp_endpoint_flush(p.ea, p.net.now_ns);
  EXPECT_TRUE(p.net.packets.empty());
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

TEST(CoopRudp, BorrowInvalidArgsReturnErrorWithoutConsumingEvent) {
  Pair p;
  const char msg[] = "borrow-valid-after-invalid";
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE;
  ASSERT_EQ(rudp_send(p.ca, msg, sizeof(msg), &opts), RUDP_SEND_QUEUED);
  p.pump();

  const void* data = nullptr;
  size_t len = 0;
  rudp_recv_info info{};
  EXPECT_EQ(rudp_recv_borrow(nullptr, &data, &len, &info), -1);
  EXPECT_EQ(rudp_recv_borrow(p.eb, nullptr, &len, &info), -1);
  EXPECT_EQ(rudp_recv_borrow(p.eb, &data, nullptr, &info), -1);

  ASSERT_EQ(rudp_recv_borrow(p.eb, &data, &len, &info), 1);
  ASSERT_NE(data, nullptr);
  EXPECT_EQ(len, sizeof(msg));
  EXPECT_EQ(std::memcmp(data, msg, sizeof(msg)), 0);
}

TEST(CoopRudp, BorrowMetaInvalidArgsReturnErrorWithoutConsumingEvent) {
  Pair p;
  const char msg[] = "borrow-meta-valid-after-invalid";
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE;
  ASSERT_EQ(rudp_send(p.ca, msg, sizeof(msg), &opts), RUDP_SEND_QUEUED);
  p.pump();

  const void* data = nullptr;
  size_t len = 0;
  uint64_t conn_id = 0;
  rudp_reliability reliability = RUDP_RELIABLE_ORDERED;
  EXPECT_EQ(rudp_recv_borrow_meta(nullptr, &data, &len, &conn_id,
                                  &reliability), -1);
  EXPECT_EQ(rudp_recv_borrow_meta(p.eb, nullptr, &len, &conn_id,
                                  &reliability), -1);
  EXPECT_EQ(rudp_recv_borrow_meta(p.eb, &data, nullptr, &conn_id,
                                  &reliability), -1);

  ASSERT_EQ(rudp_recv_borrow_meta(p.eb, &data, &len, &conn_id,
                                  &reliability), 1);
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

TEST(CoopRudp, FlowQueueLimitAllowsExactBytesAndRejectsNext) {
  Pair p;
  rudp_flow_limit lim{};
  lim.flow_id = 1;
  lim.max_bps = UINT32_MAX;
  lim.max_queue_bytes = 10;
  rudp_set_flow_limits(p.ca, &lim, 1);

  std::array<uint8_t, 5> payload{};
  rudp_send_opts opts{};
  opts.flow_id = 1;
  opts.reliability = RUDP_UNRELIABLE;
  EXPECT_EQ(rudp_send(p.ca, payload.data(), payload.size(), &opts),
            RUDP_SEND_QUEUED);
  EXPECT_EQ(rudp_send(p.ca, payload.data(), payload.size(), &opts),
            RUDP_SEND_QUEUED);
  EXPECT_EQ(rudp_send(p.ca, payload.data(), 1, &opts), RUDP_SEND_QUEUE_FULL);
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

TEST(CoopRudp, ReliableOrderedDeadlineDoesNotCreatePermanentGap) {
  Pair p;
  const char first[] = "expired-but-ordered";
  const char second[] = "after-ordered";

  rudp_send_opts opts{};
  opts.reliability = RUDP_RELIABLE_ORDERED;
  opts.deadline_ms = 1;
  ASSERT_EQ(rudp_send(p.ca, first, sizeof(first), &opts), RUDP_SEND_QUEUED);

  opts.deadline_ms = 0;
  ASSERT_EQ(rudp_send(p.ca, second, sizeof(second), &opts), RUDP_SEND_QUEUED);

  p.net.now_ns += 2'000'000ull;
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_FALSE(p.net.packets.empty());
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  char out[64]{};
  size_t len = 0;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_EQ(len, sizeof(first));
  EXPECT_EQ(std::memcmp(out, first, sizeof(first)), 0);

  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_EQ(len, sizeof(second));
  EXPECT_EQ(std::memcmp(out, second, sizeof(second)), 0);
}

TEST(CoopRudp, SendDeadlineUsesCurrentCallbackTimeAfterFlush) {
  Pair p;
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_TRUE(p.net.packets.empty());

  p.net.now_ns += 100'000'000ull;
  const char msg[] = "fresh-deadline";
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE;
  opts.deadline_ms = 10;
  ASSERT_EQ(rudp_send(p.ca, msg, sizeof(msg), &opts), RUDP_SEND_QUEUED);

  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_EQ(p.net.packets.size(), 1u);
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  char out[32]{};
  size_t len = 0;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_STREQ(out, msg);
}

TEST(CoopRudp, SendDeadlineOverflowDoesNotExpireImmediately) {
  Pair p;
  p.net.now_ns = UINT64_MAX - 1'000'000ull;
  const char msg[] = "future";
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE;
  opts.deadline_ms = 10;
  ASSERT_EQ(rudp_send(p.ca, msg, sizeof(msg), &opts), RUDP_SEND_QUEUED);

  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_EQ(p.net.packets.size(), 1u);
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  char out[32]{};
  size_t len = 0;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_STREQ(out, msg);
}

TEST(CoopRudp, ReplaceKeyDropsOlderQueuedUnreliable) {
  Pair p;
  const char old_msg[] = "old-state";
  const char new_msg[] = "new-state";
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE_SEQUENCED;
  opts.replace_key = 7;
  ASSERT_EQ(rudp_send(p.ca, old_msg, sizeof(old_msg), &opts), RUDP_SEND_QUEUED);
  ASSERT_EQ(rudp_send(p.ca, new_msg, sizeof(new_msg), &opts), RUDP_SEND_QUEUED);

  p.pump();
  char out[32]{};
  size_t len = 0;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_STREQ(out, new_msg);
  EXPECT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 0);

  rudp_flow_stats stats[8]{};
  ASSERT_GE(rudp_get_flow_stats(p.ca, stats, 8), 1u);
  EXPECT_EQ(stats[0].send_dropped, 1u);
}

TEST(CoopRudp, FailedOversizedReplacementKeepsOlderQueuedMessage) {
  Pair p(/*mtu=*/54, /*max_payload=*/300);
  const uint8_t old_msg = 0x42;
  std::array<uint8_t, 301> oversized{};
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE_SEQUENCED;
  opts.replace_key = 9;
  ASSERT_EQ(rudp_send(p.ca, &old_msg, 1, &opts), RUDP_SEND_QUEUED);
  EXPECT_EQ(rudp_send(p.ca, oversized.data(), oversized.size(), &opts),
            RUDP_SEND_QUEUE_FULL);

  p.pump();
  uint8_t out = 0;
  size_t len = 0;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv(p.eb, &out, sizeof(out), &len, &info), 1);
  EXPECT_EQ(len, 1u);
  EXPECT_EQ(out, old_msg);
}

TEST(CoopRudp, FailedQueueLimitReplacementKeepsOlderQueuedMessage) {
  Pair p;
  const uint8_t old_msg = 0x24;
  std::array<uint8_t, 5> too_large{};
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE_SEQUENCED;
  opts.replace_key = 11;
  ASSERT_EQ(rudp_send(p.ca, &old_msg, 1, &opts), RUDP_SEND_QUEUED);

  rudp_flow_limit lim{};
  lim.flow_id = 0;
  lim.max_bps = UINT32_MAX;
  lim.max_queue_bytes = 4;
  rudp_set_flow_limits(p.ca, &lim, 1);
  EXPECT_EQ(rudp_send(p.ca, too_large.data(), too_large.size(), &opts),
            RUDP_SEND_QUEUE_FULL);

  p.pump();
  uint8_t out = 0;
  size_t len = 0;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv(p.eb, &out, sizeof(out), &len, &info), 1);
  EXPECT_EQ(len, 1u);
  EXPECT_EQ(out, old_msg);
}

TEST(CoopRudp, FailedCapacityReplacementKeepsOlderQueuedMessage) {
  FakeNet net;
  FakeSock a{&net, addr(1)};
  FakeSock b{&net, addr(2)};
  rudp_endpoint_config ca_cfg = config_for(&a, /*mtu=*/64, /*max_payload=*/24);
  ca_cfg.max_conns = 1;
  ca_cfg.max_flows = 1;
  ca_cfg.max_channels = 1;
  ca_cfg.max_messages = 2;
  rudp_endpoint_config cb_cfg = config_for(&b, /*mtu=*/64, /*max_payload=*/24);
  cb_cfg.max_conns = 1;
  cb_cfg.max_flows = 1;
  cb_cfg.max_channels = 1;

  rudp_endpoint* ea = nullptr;
  rudp_endpoint* eb = nullptr;
  ASSERT_EQ(rudp_endpoint_create(&ea, &ca_cfg), 0);
  ASSERT_EQ(rudp_endpoint_create(&eb, &cb_cfg), 0);
  rudp_conn* ca = nullptr;
  rudp_addr peer = addr(2);
  ASSERT_EQ(rudp_endpoint_connect(ea, &peer, 42, &ca), 0);

  const uint8_t old_msg = 0x11;
  const uint8_t other_msg = 0x22;
  std::array<uint8_t, 24> replacement{};
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE_SEQUENCED;
  opts.replace_key = 5;
  ASSERT_EQ(rudp_send(ca, &old_msg, 1, &opts), RUDP_SEND_QUEUED);
  opts.replace_key = 0;
  ASSERT_EQ(rudp_send(ca, &other_msg, 1, &opts), RUDP_SEND_QUEUED);
  opts.replace_key = 5;
  EXPECT_EQ(rudp_send(ca, replacement.data(), replacement.size(), &opts),
            RUDP_SEND_QUEUE_FULL);

  rudp_endpoint_flush(ea, net.now_ns);
  rudp_endpoint_poll(eb, net.now_ns);
  uint8_t out = 0;
  size_t len = 0;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv(eb, &out, sizeof(out), &len, &info), 1);
  EXPECT_EQ(len, 1u);
  EXPECT_EQ(out, old_msg);
  ASSERT_EQ(rudp_recv(eb, &out, sizeof(out), &len, &info), 1);
  EXPECT_EQ(len, 1u);
  EXPECT_EQ(out, other_msg);
  EXPECT_EQ(rudp_recv(eb, &out, sizeof(out), &len, &info), 0);

  rudp_endpoint_destroy(ea);
  rudp_endpoint_destroy(eb);
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

TEST(CoopRudp, ConnAbortReleasesQueuedReliableMessages) {
  Pair p;
  const char msg[] = "abort-me";
  rudp_send_opts opts{};
  opts.reliability = RUDP_RELIABLE_UNORDERED;
  ASSERT_EQ(rudp_send(p.ca, msg, sizeof(msg), &opts), RUDP_SEND_QUEUED);

  rudp_status st{};
  rudp_get_status(p.ca, &st);
  ASSERT_EQ(st.send_queue_bytes, sizeof(msg));

  rudp_conn_abort(p.ca);
  EXPECT_EQ(rudp_endpoint_find_conn(p.ea, 42), nullptr);
  rudp_get_status(p.ca, &st);
  EXPECT_EQ(st.usable, 0u);
  EXPECT_EQ(st.send_queue_bytes, 0u);
  EXPECT_EQ(st.inflight_bytes, 0u);
  EXPECT_EQ(st.retransmit_queue_bytes, 0u);
  EXPECT_EQ(rudp_send(p.ca, msg, sizeof(msg), &opts), RUDP_SEND_UNUSABLE);

  rudp_endpoint_flush(p.ea, p.net.now_ns);
  EXPECT_TRUE(p.net.packets.empty());
}

TEST(CoopRudp, MaxRetransmitsAbortsConnectionAndFreesInflight) {
  Pair p(/*mtu=*/512, /*max_payload=*/0, /*send_batch_size=*/16,
         /*max_recv_events=*/64, /*max_ordered_holds=*/32,
         /*sent_packet_count=*/128, /*max_retransmits=*/1);
  const char msg[] = "retry-timeout";
  rudp_send_opts opts{};
  opts.reliability = RUDP_RELIABLE_UNORDERED;
  ASSERT_EQ(rudp_send(p.ca, msg, sizeof(msg), &opts), RUDP_SEND_QUEUED);

  p.net.drop_next = true;
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  rudp_status st{};
  rudp_get_status(p.ca, &st);
  ASSERT_EQ(st.inflight_bytes, sizeof(msg));

  // max_retransmits は「受信なしで許容する RTO 再送ラウンド数」。ラウンド 1
  // （t=25ms、rto=20ms）は再送して usable のまま。
  p.net.now_ns += 25'000'000ull;
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  p.net.packets.clear();
  rudp_get_status(p.ca, &st);
  ASSERT_EQ(st.usable, 1u);
  ASSERT_EQ(st.inflight_bytes, sizeof(msg));

  // ラウンド 1 で RTO は指数バックオフ（20ms→40ms）するので、ラウンド 2 の
  // 判定は t=25+40ms 以降。max_retransmits=1 を超過して abort する。
  p.net.now_ns += 45'000'000ull;
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  EXPECT_EQ(rudp_endpoint_find_conn(p.ea, 42), nullptr);
  rudp_get_status(p.ca, &st);
  EXPECT_EQ(st.usable, 0u);
  EXPECT_EQ(st.inflight_bytes, 0u);
  EXPECT_EQ(st.retransmit_queue_bytes, 0u);
}

TEST(CoopRudp, IdleTimeoutAbortsConnectionAndFreesInflight) {
  Pair p(/*mtu=*/512, /*max_payload=*/0, /*send_batch_size=*/16,
         /*max_recv_events=*/64, /*max_ordered_holds=*/32,
         /*sent_packet_count=*/128, /*max_retransmits=*/0,
         /*idle_timeout_ms=*/10);
  const char msg[] = "idle-timeout";
  rudp_send_opts opts{};
  opts.reliability = RUDP_RELIABLE_UNORDERED;
  ASSERT_EQ(rudp_send(p.ca, msg, sizeof(msg), &opts), RUDP_SEND_QUEUED);

  p.net.drop_next = true;
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  rudp_status st{};
  rudp_get_status(p.ca, &st);
  ASSERT_EQ(st.inflight_bytes, sizeof(msg));

  p.net.now_ns += 11'000'000ull;
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  EXPECT_EQ(rudp_endpoint_find_conn(p.ea, 42), nullptr);
  rudp_get_status(p.ca, &st);
  EXPECT_EQ(st.usable, 0u);
  EXPECT_EQ(st.inflight_bytes, 0u);
  EXPECT_EQ(st.retransmit_queue_bytes, 0u);
}

TEST(CoopRudp, ReliableRetransmitsWhenRecvQueueWasFull) {
  Pair p(/*mtu=*/512, /*max_payload=*/0, /*send_batch_size=*/16,
         /*max_recv_events=*/1);
  const char filler[] = "filler";
  const char msg[] = "must-arrive";
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE;
  ASSERT_EQ(rudp_send(p.ca, filler, sizeof(filler), &opts), RUDP_SEND_QUEUED);
  p.pump();

  opts.reliability = RUDP_RELIABLE_UNORDERED;
  ASSERT_EQ(rudp_send(p.ca, msg, sizeof(msg), &opts), RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  rudp_endpoint_poll(p.eb, p.net.now_ns);
  rudp_endpoint_flush(p.eb, p.net.now_ns);
  rudp_endpoint_poll(p.ea, p.net.now_ns);

  rudp_status st{};
  rudp_get_status(p.ca, &st);
  EXPECT_EQ(st.inflight_bytes, sizeof(msg));

  char out[32]{};
  size_t len = 0;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_STREQ(out, filler);
  EXPECT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 0);

  p.net.now_ns += 25'000'000ull;
  p.pump();
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_STREQ(out, msg);
}

TEST(CoopRudp, ReliableAckClearsInflightAndUpdatesStatus) {
  Pair p;
  const char msg[] = "ack-me";
  rudp_send_opts opts{};
  opts.reliability = RUDP_RELIABLE_UNORDERED;
  ASSERT_EQ(rudp_send(p.ca, msg, sizeof(msg), &opts), RUDP_SEND_QUEUED);

  rudp_endpoint_flush(p.ea, p.net.now_ns);
  rudp_status st{};
  rudp_get_status(p.ca, &st);
  EXPECT_EQ(st.inflight_bytes, sizeof(msg));
  EXPECT_EQ(st.send_queue_bytes, 0u);

  p.net.now_ns += 1'000'000ull;
  rudp_endpoint_poll(p.eb, p.net.now_ns);
  rudp_endpoint_flush(p.eb, p.net.now_ns);
  rudp_endpoint_poll(p.ea, p.net.now_ns);

  rudp_get_status(p.ca, &st);
  EXPECT_EQ(st.inflight_bytes, 0u);
  EXPECT_EQ(st.retransmit_queue_bytes, 0u);
  EXPECT_GE(st.rtt_ms, 1u);

  rudp_flow_stats stats[8]{};
  ASSERT_GE(rudp_get_flow_stats(p.ca, stats, 8), 1u);
  EXPECT_EQ(stats[0].delivered_bps, sizeof(msg));
}

TEST(CoopRudp, LostReliableRetransmitDoesNotLeakInflight) {
  Pair p;
  const char msg[] = "retry-me";
  rudp_send_opts opts{};
  opts.reliability = RUDP_RELIABLE_UNORDERED;
  ASSERT_EQ(rudp_send(p.ca, msg, sizeof(msg), &opts), RUDP_SEND_QUEUED);

  p.net.drop_next = true;
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  rudp_status st{};
  rudp_get_status(p.ca, &st);
  EXPECT_EQ(st.inflight_bytes, sizeof(msg));

  p.net.now_ns += 25'000'000ull;
  p.pump();

  rudp_get_status(p.ca, &st);
  EXPECT_EQ(st.inflight_bytes, 0u);
  EXPECT_EQ(st.retransmit_queue_bytes, 0u);
  EXPECT_GT(st.loss_ppm, 0u);

  rudp_flow_stats stats[8]{};
  ASSERT_GE(rudp_get_flow_stats(p.ca, stats, 8), 1u);
  EXPECT_EQ(stats[0].retransmits, 1u);
}

TEST(CoopRudp, PendingReliablePacketsReserveSentSlotsWithinBatch) {
  Pair p(/*mtu=*/64, /*max_payload=*/0, /*send_batch_size=*/16,
         /*max_recv_events=*/64, /*max_ordered_holds=*/32,
         /*sent_packet_count=*/1);
  std::array<uint8_t, 12> first{};
  std::array<uint8_t, 12> second{};
  first.fill(0x31);
  second.fill(0x32);
  rudp_send_opts opts{};
  opts.reliability = RUDP_RELIABLE_UNORDERED;
  ASSERT_EQ(rudp_send(p.ca, first.data(), first.size(), &opts),
            RUDP_SEND_QUEUED);
  ASSERT_EQ(rudp_send(p.ca, second.data(), second.size(), &opts),
            RUDP_SEND_QUEUED);

  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_EQ(p.net.packets.size(), 1u);

  rudp_status st{};
  rudp_get_status(p.ca, &st);
  EXPECT_EQ(st.inflight_bytes, first.size());
  EXPECT_EQ(st.send_queue_bytes, second.size());

  rudp_endpoint_poll(p.eb, p.net.now_ns);
  std::array<uint8_t, 12> out{};
  size_t len = 0;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv(p.eb, out.data(), out.size(), &len, &info), 1);
  EXPECT_EQ(out, first);

  rudp_endpoint_flush(p.eb, p.net.now_ns);
  rudp_endpoint_poll(p.ea, p.net.now_ns);
  rudp_get_status(p.ca, &st);
  EXPECT_EQ(st.inflight_bytes, 0u);
  EXPECT_EQ(st.send_queue_bytes, second.size());

  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_EQ(p.net.packets.size(), 1u);
  rudp_endpoint_poll(p.eb, p.net.now_ns);
  ASSERT_EQ(rudp_recv(p.eb, out.data(), out.size(), &len, &info), 1);
  EXPECT_EQ(out, second);
}

TEST(CoopRudp, RateLimitedRetryPreventsNewMessagesFromBypassing) {
  Pair p(/*mtu=*/512, /*max_payload=*/1500);
  rudp_flow_limit lim{};
  lim.flow_id = 0;
  lim.max_bps = 80;
  lim.max_queue_bytes = UINT32_MAX;
  rudp_set_flow_limits(p.ca, &lim, 1);

  std::array<uint8_t, 1400> big{};
  big.fill(0x7a);
  const uint8_t small = 0x11;
  rudp_send_opts opts{};
  opts.reliability = RUDP_RELIABLE_UNORDERED;
  ASSERT_EQ(rudp_send(p.ca, big.data(), big.size(), &opts), RUDP_SEND_QUEUED);

  p.net.drop_first_packet_once = true;
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  EXPECT_FALSE(p.net.packets.empty());
  p.net.packets.clear();

  p.net.now_ns += 25'000'000ull;
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  rudp_status st{};
  rudp_get_status(p.ca, &st);
  ASSERT_EQ(st.retransmit_queue_bytes, big.size());

  ASSERT_EQ(rudp_send(p.ca, &small, sizeof(small), &opts), RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  EXPECT_TRUE(p.net.packets.empty());

  rudp_get_status(p.ca, &st);
  EXPECT_EQ(st.retransmit_queue_bytes, big.size());
  EXPECT_EQ(st.send_queue_bytes, sizeof(small));
}

TEST(CoopRudp, AckGapRetransmitsBeforeRto) {
  Pair p(/*mtu=*/64);
  std::array<uint8_t, 12> first{};
  std::array<uint8_t, 12> other{};
  first.fill(0x11);
  other.fill(0x22);
  rudp_send_opts opts{};
  opts.reliability = RUDP_RELIABLE_UNORDERED;
  ASSERT_EQ(rudp_send(p.ca, first.data(), first.size(), &opts), RUDP_SEND_QUEUED);
  for (int i = 0; i < 3; ++i) {
    other[0] = static_cast<uint8_t>(0x30 + i);
    ASSERT_EQ(rudp_send(p.ca, other.data(), other.size(), &opts), RUDP_SEND_QUEUED);
  }

  p.net.drop_first_packet_once = true;
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_EQ(p.net.packets.size(), 3u);
  rudp_endpoint_poll(p.eb, p.net.now_ns);
  rudp_endpoint_flush(p.eb, p.net.now_ns);
  rudp_endpoint_poll(p.ea, p.net.now_ns);

  rudp_status st{};
  rudp_get_status(p.ca, &st);
  EXPECT_EQ(st.retransmit_queue_bytes, first.size());
  EXPECT_GT(st.loss_ppm, 0u);

  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_FALSE(p.net.packets.empty());
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  bool got_first = false;
  std::array<uint8_t, 12> out{};
  size_t len = 0;
  rudp_recv_info info{};
  while (rudp_recv(p.eb, out.data(), out.size(), &len, &info) == 1) {
    if (len == first.size() && out == first) got_first = true;
  }
  EXPECT_TRUE(got_first);
}

TEST(CoopRudp, LateAckCancelsQueuedRetransmit) {
  Pair p(/*mtu=*/64);
  std::array<uint8_t, 12> first{};
  std::array<uint8_t, 12> other{};
  first.fill(0x11);
  other.fill(0x22);
  rudp_send_opts opts{};
  opts.reliability = RUDP_RELIABLE_UNORDERED;
  ASSERT_EQ(rudp_send(p.ca, first.data(), first.size(), &opts), RUDP_SEND_QUEUED);
  for (int i = 0; i < 3; ++i) {
    other[0] = static_cast<uint8_t>(0x30 + i);
    ASSERT_EQ(rudp_send(p.ca, other.data(), other.size(), &opts), RUDP_SEND_QUEUED);
  }

  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_EQ(p.net.packets.size(), 4u);
  Packet delayed_first = std::move(p.net.packets.front());
  p.net.packets.pop_front();

  rudp_endpoint_poll(p.eb, p.net.now_ns);
  rudp_endpoint_flush(p.eb, p.net.now_ns);
  rudp_endpoint_poll(p.ea, p.net.now_ns);

  rudp_status st{};
  rudp_get_status(p.ca, &st);
  ASSERT_EQ(st.retransmit_queue_bytes, first.size());

  p.net.packets.push_back(std::move(delayed_first));
  rudp_endpoint_poll(p.eb, p.net.now_ns);
  rudp_endpoint_flush(p.eb, p.net.now_ns);
  rudp_endpoint_poll(p.ea, p.net.now_ns);

  rudp_get_status(p.ca, &st);
  EXPECT_EQ(st.retransmit_queue_bytes, 0u);
  EXPECT_EQ(st.inflight_bytes, 0u);

  rudp_endpoint_flush(p.ea, p.net.now_ns);
  EXPECT_TRUE(p.net.packets.empty());
}

TEST(CoopRudp, LateAckCancelsInflightRetransmit) {
  Pair p(/*mtu=*/64);
  std::array<uint8_t, 12> first{};
  std::array<uint8_t, 12> other{};
  first.fill(0x11);
  other.fill(0x22);
  rudp_send_opts opts{};
  opts.reliability = RUDP_RELIABLE_UNORDERED;
  ASSERT_EQ(rudp_send(p.ca, first.data(), first.size(), &opts), RUDP_SEND_QUEUED);
  for (int i = 0; i < 3; ++i) {
    other[0] = static_cast<uint8_t>(0x30 + i);
    ASSERT_EQ(rudp_send(p.ca, other.data(), other.size(), &opts), RUDP_SEND_QUEUED);
  }

  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_EQ(p.net.packets.size(), 4u);
  Packet delayed_first = std::move(p.net.packets.front());
  p.net.packets.pop_front();

  rudp_endpoint_poll(p.eb, p.net.now_ns);
  rudp_endpoint_flush(p.eb, p.net.now_ns);
  rudp_endpoint_poll(p.ea, p.net.now_ns);

  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_EQ(p.net.packets.size(), 1u);
  Packet delayed_retry = std::move(p.net.packets.front());
  p.net.packets.pop_front();

  rudp_status st{};
  rudp_get_status(p.ca, &st);
  ASSERT_EQ(st.retransmit_queue_bytes, 0u);
  ASSERT_EQ(st.inflight_bytes, first.size());

  p.net.packets.push_back(std::move(delayed_first));
  rudp_endpoint_poll(p.eb, p.net.now_ns);
  rudp_endpoint_flush(p.eb, p.net.now_ns);
  rudp_endpoint_poll(p.ea, p.net.now_ns);

  rudp_get_status(p.ca, &st);
  EXPECT_EQ(st.retransmit_queue_bytes, 0u);
  EXPECT_EQ(st.inflight_bytes, 0u);

  p.net.packets.push_back(std::move(delayed_retry));
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  std::array<uint8_t, 12> out{};
  size_t len = 0;
  rudp_recv_info info{};
  size_t first_count = 0;
  while (rudp_recv(p.eb, out.data(), out.size(), &len, &info) == 1) {
    if (len == first.size() && out == first) ++first_count;
  }
  EXPECT_EQ(first_count, 1u);
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

TEST(CoopRudp, IncompleteReassemblyExpires) {
  Pair p(/*mtu=*/96, /*max_payload=*/160);
  std::array<uint8_t, 130> msg{};
  for (size_t i = 0; i < msg.size(); ++i) msg[i] = static_cast<uint8_t>(i + 3);
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

  p.net.now_ns += 150'000'000ull;
  rudp_endpoint_poll(p.eb, p.net.now_ns);
  p.net.packets.push_back(std::move(held));
  rudp_endpoint_poll(p.eb, p.net.now_ns);
  EXPECT_EQ(rudp_recv(p.eb, out.data(), out.size(), &len, &info), 0);
}

TEST(CoopRudp, DroppedUnreliableReassemblyFreesSlot) {
  Pair p(/*mtu=*/96, /*max_payload=*/160, /*send_batch_size=*/16,
         /*max_recv_events=*/1);
  const char filler[] = "filler";
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE;
  ASSERT_EQ(rudp_send(p.ca, filler, sizeof(filler), &opts), RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  std::array<uint8_t, 80> dropped{};
  dropped.fill('d');
  ASSERT_EQ(rudp_send(p.ca, dropped.data(), dropped.size(), &opts),
            RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_FALSE(p.net.packets.empty());
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  std::array<uint8_t, 160> out{};
  size_t len = 0;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv(p.eb, out.data(), out.size(), &len, &info), 1);
  ASSERT_EQ(len, sizeof(filler));
  EXPECT_EQ(std::memcmp(out.data(), filler, sizeof(filler)), 0);

  std::array<uint8_t, 80> next{};
  next.fill('n');
  ASSERT_EQ(rudp_send(p.ca, next.data(), next.size(), &opts), RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_FALSE(p.net.packets.empty());
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  ASSERT_EQ(rudp_recv(p.eb, out.data(), out.size(), &len, &info), 1);
  EXPECT_EQ(len, next.size());
  EXPECT_EQ(std::memcmp(out.data(), next.data(), next.size()), 0);
}

TEST(CoopRudp, MalformedReliableFragmentDoesNotApplyAck) {
  Pair p(/*mtu=*/96, /*max_payload=*/160);
  const char msg[] = "ack-target";
  rudp_send_opts opts{};
  opts.reliability = RUDP_RELIABLE_UNORDERED;
  ASSERT_EQ(rudp_send(p.ca, msg, sizeof(msg), &opts), RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  p.net.packets.clear();

  Packet bad;
  bad.from = addr(2);
  bad.to = addr(1);
  bad.bytes = make_fragment_packet(kReliableFlag, /*packet_seq=*/7,
                                   /*msg_id=*/222, /*frag_index=*/0,
                                   /*frag_count=*/2, /*payload_len=*/1,
                                   /*fill=*/0xaa);
  store_u32_le(bad.bytes, 16, 1);
  p.net.packets.push_back(std::move(bad));

  rudp_endpoint_poll(p.ea, p.net.now_ns);
  rudp_status st{};
  rudp_get_status(p.ca, &st);
  EXPECT_EQ(st.inflight_bytes, sizeof(msg));
}

TEST(CoopRudp, ImpossibleFragmentCountDoesNotApplyAck) {
  Pair p(/*mtu=*/96, /*max_payload=*/160);
  const char msg[] = "ack-target";
  rudp_send_opts opts{};
  opts.reliability = RUDP_RELIABLE_UNORDERED;
  ASSERT_EQ(rudp_send(p.ca, msg, sizeof(msg), &opts), RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  p.net.packets.clear();

  Packet bad;
  bad.from = addr(2);
  bad.to = addr(1);
  bad.bytes = make_fragment_packet(kReliableFlag, /*packet_seq=*/7,
                                   /*msg_id=*/223, /*frag_index=*/0,
                                   /*frag_count=*/5, /*payload_len=*/44,
                                   /*fill=*/0xaa);
  store_u32_le(bad.bytes, 16, 1);
  p.net.packets.push_back(std::move(bad));

  rudp_endpoint_poll(p.ea, p.net.now_ns);
  rudp_status st{};
  rudp_get_status(p.ca, &st);
  EXPECT_EQ(st.inflight_bytes, sizeof(msg));
}

TEST(CoopRudp, ReassemblyRejectsMixedReliabilityFragments) {
  Pair p(/*mtu=*/96, /*max_payload=*/160);
  constexpr uint16_t first_len = 44;
  constexpr uint16_t last_len = 3;

  Packet first;
  first.from = addr(1);
  first.to = addr(2);
  first.bytes = make_fragment_packet(kReliableFlag, /*packet_seq=*/7,
                                     /*msg_id=*/333, /*frag_index=*/0,
                                     /*frag_count=*/2, first_len, 'A');
  Packet wrong;
  wrong.from = addr(1);
  wrong.to = addr(2);
  wrong.bytes = make_fragment_packet(/*flags=*/0, /*packet_seq=*/8,
                                     /*msg_id=*/333, /*frag_index=*/1,
                                     /*frag_count=*/2, last_len, 'B');
  p.net.packets.push_back(std::move(first));
  p.net.packets.push_back(std::move(wrong));
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  std::array<uint8_t, 64> out{};
  size_t len = 0;
  rudp_recv_info info{};
  EXPECT_EQ(rudp_recv(p.eb, out.data(), out.size(), &len, &info), 0);

  Packet last;
  last.from = addr(1);
  last.to = addr(2);
  last.bytes = make_fragment_packet(kReliableFlag, /*packet_seq=*/9,
                                    /*msg_id=*/333, /*frag_index=*/1,
                                    /*frag_count=*/2, last_len, 'C');
  p.net.packets.push_back(std::move(last));
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  ASSERT_EQ(rudp_recv(p.eb, out.data(), out.size(), &len, &info), 1);
  ASSERT_EQ(len, first_len + last_len);
  EXPECT_EQ(std::count(out.begin(), out.begin() + first_len, 'A'),
            first_len);
  EXPECT_EQ(std::count(out.begin() + first_len, out.begin() + len, 'C'),
            last_len);
}

TEST(CoopRudp, ReassemblySemanticMismatchDoesNotApplyAck) {
  Pair p(/*mtu=*/96, /*max_payload=*/160);
  const char msg[] = "ack-target";
  rudp_send_opts opts{};
  opts.reliability = RUDP_RELIABLE_UNORDERED;
  ASSERT_EQ(rudp_send(p.ca, msg, sizeof(msg), &opts), RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  p.net.packets.clear();

  Packet first;
  first.from = addr(2);
  first.to = addr(1);
  first.bytes = make_fragment_packet(kReliableFlag, /*packet_seq=*/7,
                                     /*msg_id=*/444, /*frag_index=*/0,
                                     /*frag_count=*/2, /*payload_len=*/44,
                                     /*fill=*/0xaa);
  p.net.packets.push_back(std::move(first));
  rudp_endpoint_poll(p.ea, p.net.now_ns);

  Packet wrong_channel;
  wrong_channel.from = addr(2);
  wrong_channel.to = addr(1);
  wrong_channel.bytes = make_fragment_packet(kReliableFlag, /*packet_seq=*/8,
                                             /*msg_id=*/444, /*frag_index=*/1,
                                             /*frag_count=*/2, /*payload_len=*/3,
                                             /*fill=*/0xbb);
  store_u32_le(wrong_channel.bytes, 16, 1);
  store_u16_le(wrong_channel.bytes, kPacketHeaderBytes + 16,
               /*channel_id=*/1);
  p.net.packets.push_back(std::move(wrong_channel));
  rudp_endpoint_poll(p.ea, p.net.now_ns);

  rudp_status st{};
  rudp_get_status(p.ca, &st);
  EXPECT_EQ(st.inflight_bytes, sizeof(msg));

  Packet wrong_reliability;
  wrong_reliability.from = addr(2);
  wrong_reliability.to = addr(1);
  wrong_reliability.bytes = make_fragment_packet(/*flags=*/0, /*packet_seq=*/9,
                                                 /*msg_id=*/444,
                                                 /*frag_index=*/1,
                                                 /*frag_count=*/2,
                                                 /*payload_len=*/3,
                                                 /*fill=*/0xcc);
  store_u32_le(wrong_reliability.bytes, 16, 1);
  p.net.packets.push_back(std::move(wrong_reliability));
  rudp_endpoint_poll(p.ea, p.net.now_ns);

  rudp_get_status(p.ca, &st);
  EXPECT_EQ(st.inflight_bytes, sizeof(msg));
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

TEST(CoopRudp, SequencedAcceptsWrapFromMaxToZero) {
  Pair p(/*mtu=*/96);
  Packet last;
  last.from = addr(1);
  last.to = addr(2);
  last.bytes = make_single_frame_packet(kSequencedFlag, /*payload_len=*/1,
                                        /*channel_seq=*/UINT16_MAX);
  last.bytes[kPacketHeaderBytes + kDataFrameHeaderBytes] = 'z';
  Packet wrapped;
  wrapped.from = addr(1);
  wrapped.to = addr(2);
  wrapped.bytes = make_single_frame_packet(kSequencedFlag, /*payload_len=*/1,
                                           /*channel_seq=*/0);
  store_u32_le(wrapped.bytes, 12, 8);
  wrapped.bytes[kPacketHeaderBytes + kDataFrameHeaderBytes] = 'a';
  p.net.packets.push_back(std::move(last));
  p.net.packets.push_back(std::move(wrapped));

  rudp_endpoint_poll(p.eb, p.net.now_ns);
  std::array<uint8_t, 1> out{};
  size_t len = 0;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv(p.eb, out.data(), out.size(), &len, &info), 1);
  EXPECT_EQ(out[0], 'z');
  ASSERT_EQ(rudp_recv(p.eb, out.data(), out.size(), &len, &info), 1);
  EXPECT_EQ(out[0], 'a');
}

TEST(CoopRudp, SequencedStateIsPerFlowAndChannel) {
  Pair p;
  const char flow0[] = "flow0-old";
  const char flow1[] = "flow1-new";
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE_SEQUENCED;
  opts.flow_id = 0;
  ASSERT_EQ(rudp_send(p.ca, flow0, sizeof(flow0), &opts), RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  opts.flow_id = 1;
  ASSERT_EQ(rudp_send(p.ca, flow1, sizeof(flow1), &opts), RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);

  ASSERT_GE(p.net.packets.size(), 2u);
  std::swap(p.net.packets[0], p.net.packets[1]);
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  char out[32]{};
  size_t len = 0;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_STREQ(out, flow1);
  EXPECT_EQ(info.flow_id, 1u);
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_STREQ(out, flow0);
  EXPECT_EQ(info.flow_id, 0u);
}

TEST(CoopRudp, NonOrderedSendsDoNotAdvanceReliableOrderedSequence) {
  Pair p;
  const char unreliable[] = "unreliable";
  const char reliable_unordered[] = "reliable-unordered";
  const char sequenced[] = "sequenced";
  const char ordered[] = "ordered";

  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE;
  ASSERT_EQ(rudp_send(p.ca, unreliable, sizeof(unreliable), &opts),
            RUDP_SEND_QUEUED);
  opts.reliability = RUDP_RELIABLE_UNORDERED;
  ASSERT_EQ(rudp_send(p.ca, reliable_unordered, sizeof(reliable_unordered),
                      &opts),
            RUDP_SEND_QUEUED);
  opts.reliability = RUDP_UNRELIABLE_SEQUENCED;
  ASSERT_EQ(rudp_send(p.ca, sequenced, sizeof(sequenced), &opts),
            RUDP_SEND_QUEUED);
  opts.reliability = RUDP_RELIABLE_ORDERED;
  ASSERT_EQ(rudp_send(p.ca, ordered, sizeof(ordered), &opts),
            RUDP_SEND_QUEUED);

  p.pump();
  char out[64]{};
  size_t len = 0;
  rudp_recv_info info{};
  int delivered = 0;
  bool saw_ordered = false;
  while (rudp_recv(p.eb, out, sizeof(out), &len, &info) == 1) {
    ++delivered;
    if (std::strcmp(out, ordered) == 0) saw_ordered = true;
  }
  EXPECT_EQ(delivered, 4);
  EXPECT_TRUE(saw_ordered);
}

TEST(CoopRudp, DroppedSequencedStillSuppressesOlderPackets) {
  Pair p(/*mtu=*/512, /*max_payload=*/0, /*send_batch_size=*/16,
         /*max_recv_events=*/1);
  const char filler[] = "filler";
  const char old_msg[] = "old-seq";
  const char new_msg[] = "new-seq";
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE;
  ASSERT_EQ(rudp_send(p.ca, filler, sizeof(filler), &opts), RUDP_SEND_QUEUED);
  p.pump();

  opts.reliability = RUDP_UNRELIABLE_SEQUENCED;
  ASSERT_EQ(rudp_send(p.ca, old_msg, sizeof(old_msg), &opts), RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_EQ(rudp_send(p.ca, new_msg, sizeof(new_msg), &opts), RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);

  ASSERT_GE(p.net.packets.size(), 2u);
  Packet held_old = std::move(p.net.packets.front());
  p.net.packets.pop_front();
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  char out[32]{};
  size_t len = 0;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_STREQ(out, filler);

  p.net.packets.push_back(std::move(held_old));
  rudp_endpoint_poll(p.eb, p.net.now_ns);
  EXPECT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 0);
}

TEST(CoopRudp, ReorderedPacketsUpdateStatus) {
  Pair p;
  const char first[] = "first-packet";
  const char second[] = "second-packet";
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE;
  ASSERT_EQ(rudp_send(p.ca, first, sizeof(first), &opts), RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_EQ(rudp_send(p.ca, second, sizeof(second), &opts), RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);

  ASSERT_GE(p.net.packets.size(), 2u);
  std::swap(p.net.packets[0], p.net.packets[1]);
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  rudp_conn* inbound = rudp_endpoint_find_conn(p.eb, 42);
  ASSERT_NE(inbound, nullptr);
  rudp_status st{};
  rudp_get_status(inbound, &st);
  EXPECT_GT(st.reorder_ppm, 0u);
}

TEST(CoopRudp, AckBitShiftAtSixtyFourKeepsOnlyRepresentablePackets) {
  Pair p(/*mtu=*/64);
  std::array<uint8_t, 12> msg{};
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE;
  for (uint8_t i = 0; i < 66; ++i) {
    msg.fill(i);
    ASSERT_EQ(rudp_send(p.ca, msg.data(), msg.size(), &opts), RUDP_SEND_QUEUED);
  }
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_EQ(p.net.packets.size(), 66u);

  Packet seq1 = std::move(p.net.packets[0]);
  Packet seq2 = std::move(p.net.packets[1]);
  Packet seq66 = std::move(p.net.packets[65]);
  p.net.packets.clear();
  p.net.packets.push_back(std::move(seq1));
  p.net.packets.push_back(std::move(seq2));
  p.net.packets.push_back(std::move(seq66));

  rudp_endpoint_poll(p.eb, p.net.now_ns);
  rudp_endpoint_flush(p.eb, p.net.now_ns);
  ASSERT_EQ(p.net.packets.size(), 1u);
  ASSERT_GE(p.net.packets.front().bytes.size(), 32u);
  EXPECT_EQ(load_u32_le(p.net.packets.front().bytes, 16), 66u);
  EXPECT_EQ(load_u64_le(p.net.packets.front().bytes, 20), 1ull << 63u);
}

TEST(CoopRudp, AckTrackerAcceptsPacketSequenceWrap) {
  Pair p(/*mtu=*/96);
  Packet a;
  a.from = addr(1);
  a.to = addr(2);
  a.bytes = make_single_frame_packet(/*flags=*/0, /*payload_len=*/0,
                                     /*channel_seq=*/0);
  store_u32_le(a.bytes, 12, UINT32_MAX - 1u);
  Packet b = a;
  store_u32_le(b.bytes, 12, UINT32_MAX);
  Packet c = a;
  store_u32_le(c.bytes, 12, 1);
  p.net.packets.push_back(std::move(a));
  p.net.packets.push_back(std::move(b));
  p.net.packets.push_back(std::move(c));

  rudp_endpoint_poll(p.eb, p.net.now_ns);
  rudp_endpoint_flush(p.eb, p.net.now_ns);
  ASSERT_EQ(p.net.packets.size(), 1u);
  ASSERT_GE(p.net.packets.front().bytes.size(), 32u);
  EXPECT_EQ(load_u32_le(p.net.packets.front().bytes, 16), 1u);
  EXPECT_EQ(load_u64_le(p.net.packets.front().bytes, 20), 0b110u);
}

TEST(CoopRudp, ReliableDuplicateSuppressionSurvivesMsgIdCacheCollisions) {
  Pair p;
  rudp_send_opts opts{};
  opts.reliability = RUDP_RELIABLE_UNORDERED;

  const char first[] = "first";
  ASSERT_EQ(rudp_send(p.ca, first, sizeof(first), &opts), RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_EQ(p.net.packets.size(), 1u);
  Packet duplicate_first = p.net.packets.front();
  rudp_endpoint_poll(p.eb, p.net.now_ns);
  rudp_endpoint_flush(p.eb, p.net.now_ns);
  rudp_endpoint_poll(p.ea, p.net.now_ns);

  char out[32]{};
  size_t len = 0;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_STREQ(out, first);

  for (uint32_t i = 0; i < 1024; ++i) {
    uint32_t value = i;
    ASSERT_EQ(rudp_send(p.ca, &value, sizeof(value), &opts), RUDP_SEND_QUEUED)
        << i;
    p.pump();
    ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1) << i;
    ASSERT_EQ(len, sizeof(value)) << i;
  }

  p.net.packets.push_back(std::move(duplicate_first));
  rudp_endpoint_poll(p.eb, p.net.now_ns);
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

TEST(CoopRudp, ReliableOrderedDuplicateHoldUsesOneSlot) {
  Pair p(/*mtu=*/96);
  auto make_ordered = [](uint32_t packet_seq, uint32_t msg_id,
                         uint16_t channel_seq, const char* text) {
    size_t payload_len = std::strlen(text) + 1;
    std::vector<uint8_t> bytes = make_single_frame_packet(
        kReliableFlag | kOrderedFlag, static_cast<uint16_t>(payload_len),
        channel_seq);
    store_u32_le(bytes, 12, packet_seq);
    store_u32_le(bytes, kPacketHeaderBytes + 8, msg_id);
    std::memcpy(bytes.data() + kPacketHeaderBytes + kDataFrameHeaderBytes,
                text, payload_len);
    return bytes;
  };

  Packet held_a;
  held_a.from = addr(1);
  held_a.to = addr(2);
  held_a.bytes = make_ordered(7, 123, 1, "held-a");
  Packet held_b;
  held_b.from = addr(1);
  held_b.to = addr(2);
  held_b.bytes = make_ordered(8, 123, 1, "held-a");
  p.net.packets.push_back(std::move(held_a));
  p.net.packets.push_back(std::move(held_b));
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  char out[32]{};
  size_t len = 0;
  rudp_recv_info info{};
  EXPECT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 0);

  Packet first;
  first.from = addr(1);
  first.to = addr(2);
  first.bytes = make_ordered(9, 122, 0, "first");
  p.net.packets.push_back(std::move(first));
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_STREQ(out, "first");
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_STREQ(out, "held-a");
  EXPECT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 0);
}

TEST(CoopRudp, ReliableOrderedHoldRejectsConflictingMsgId) {
  Pair p(/*mtu=*/96);
  auto make_ordered = [](uint32_t packet_seq, uint32_t msg_id,
                         uint16_t channel_seq, const char* text) {
    size_t payload_len = std::strlen(text) + 1;
    std::vector<uint8_t> bytes = make_single_frame_packet(
        kReliableFlag | kOrderedFlag, static_cast<uint16_t>(payload_len),
        channel_seq);
    store_u32_le(bytes, 12, packet_seq);
    store_u32_le(bytes, kPacketHeaderBytes + 8, msg_id);
    std::memcpy(bytes.data() + kPacketHeaderBytes + kDataFrameHeaderBytes,
                text, payload_len);
    return bytes;
  };

  Packet held_a;
  held_a.from = addr(1);
  held_a.to = addr(2);
  held_a.bytes = make_ordered(7, 123, 1, "held-a");
  Packet held_b;
  held_b.from = addr(1);
  held_b.to = addr(2);
  held_b.bytes = make_ordered(8, 124, 1, "held-b");
  p.net.packets.push_back(std::move(held_a));
  p.net.packets.push_back(std::move(held_b));
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  rudp_endpoint_flush(p.eb, p.net.now_ns);
  ASSERT_EQ(p.net.packets.size(), 1u);
  ASSERT_GE(p.net.packets.front().bytes.size(), kPacketHeaderBytes);
  EXPECT_EQ(load_u32_le(p.net.packets.front().bytes, 16), 7u);
  p.net.packets.clear();

  Packet first;
  first.from = addr(1);
  first.to = addr(2);
  first.bytes = make_ordered(9, 122, 0, "first");
  p.net.packets.push_back(std::move(first));
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  char out[32]{};
  size_t len = 0;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_STREQ(out, "first");
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_STREQ(out, "held-a");
  EXPECT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 0);
}

TEST(CoopRudp, OversizedOutOfOrderOrderedFrameIsNotHeldOrAcked) {
  Pair p(/*mtu=*/96, /*max_payload=*/8);
  constexpr uint16_t payload_len = 16;
  std::vector<uint8_t> bytes =
      make_single_frame_packet(kReliableFlag | kOrderedFlag, payload_len, 1);

  Packet bad;
  bad.from = addr(1);
  bad.to = addr(2);
  bad.bytes = std::move(bytes);
  p.net.packets.push_back(std::move(bad));

  rudp_endpoint_poll(p.eb, p.net.now_ns);
  EXPECT_TRUE(p.net.packets.empty());
  rudp_endpoint_flush(p.eb, p.net.now_ns);
  EXPECT_TRUE(p.net.packets.empty());

  std::array<uint8_t, 32> out{};
  size_t len = 0;
  rudp_recv_info info{};
  EXPECT_EQ(rudp_recv(p.eb, out.data(), out.size(), &len, &info), 0);
}

TEST(CoopRudp, SingleFrameLargerThanFramePayloadIsRejected) {
  Pair p(/*mtu=*/64, /*max_payload=*/128);
  Packet bad;
  bad.from = addr(1);
  bad.to = addr(2);
  bad.bytes = make_single_frame_packet(/*flags=*/0, /*payload_len=*/13,
                                       /*channel_seq=*/0);
  p.net.packets.push_back(std::move(bad));

  rudp_endpoint_poll(p.eb, p.net.now_ns);
  std::array<uint8_t, 32> out{};
  size_t len = 0;
  rudp_recv_info info{};
  EXPECT_EQ(rudp_recv(p.eb, out.data(), out.size(), &len, &info), 0);
}

TEST(CoopRudp, PacketLargerThanEndpointMtuIsRejected) {
  Pair p(/*mtu=*/64, /*max_payload=*/128);
  Packet bad;
  bad.from = addr(1);
  bad.to = addr(2);
  bad.bytes = make_two_frame_packet(/*payload_len=*/1);
  ASSERT_GT(bad.bytes.size(), 64u);
  p.net.packets.push_back(std::move(bad));

  rudp_endpoint_poll(p.eb, p.net.now_ns);
  std::array<uint8_t, 32> out{};
  size_t len = 0;
  rudp_recv_info info{};
  EXPECT_EQ(rudp_recv(p.eb, out.data(), out.size(), &len, &info), 0);
}

TEST(CoopRudp, MalformedReliablePacketWithTrailingBytesHasNoDeliveryOrAck) {
  Pair p(/*mtu=*/96);
  Packet bad;
  bad.from = addr(1);
  bad.to = addr(2);
  bad.bytes = make_single_frame_packet(kReliableFlag, /*payload_len=*/4,
                                       /*channel_seq=*/0,
                                       /*trailing_bytes=*/1);
  p.net.packets.push_back(std::move(bad));

  rudp_endpoint_poll(p.eb, p.net.now_ns);
  std::array<uint8_t, 32> out{};
  size_t len = 0;
  rudp_recv_info info{};
  EXPECT_EQ(rudp_recv(p.eb, out.data(), out.size(), &len, &info), 0);

  EXPECT_TRUE(p.net.packets.empty());
  rudp_endpoint_flush(p.eb, p.net.now_ns);
  EXPECT_TRUE(p.net.packets.empty());
}

TEST(CoopRudp, MalformedPacketDoesNotApplyAck) {
  Pair p(/*mtu=*/96);
  const char msg[] = "ack-target";
  rudp_send_opts opts{};
  opts.reliability = RUDP_RELIABLE_UNORDERED;
  ASSERT_EQ(rudp_send(p.ca, msg, sizeof(msg), &opts), RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  p.net.packets.clear();

  rudp_status st{};
  rudp_get_status(p.ca, &st);
  ASSERT_EQ(st.inflight_bytes, sizeof(msg));

  Packet bad_ack;
  bad_ack.from = addr(2);
  bad_ack.to = addr(1);
  bad_ack.bytes = make_single_frame_packet(kReliableFlag, /*payload_len=*/4,
                                           /*channel_seq=*/0,
                                           /*trailing_bytes=*/1);
  store_u32_le(bad_ack.bytes, 16, 1);
  p.net.packets.push_back(std::move(bad_ack));

  rudp_endpoint_poll(p.ea, p.net.now_ns);
  rudp_get_status(p.ca, &st);
  EXPECT_EQ(st.inflight_bytes, sizeof(msg));
}

TEST(CoopRudp, PacketWithBytesAfterDeclaredPayloadDoesNotApplyAck) {
  Pair p(/*mtu=*/96);
  const char msg[] = "ack-target";
  rudp_send_opts opts{};
  opts.reliability = RUDP_RELIABLE_UNORDERED;
  ASSERT_EQ(rudp_send(p.ca, msg, sizeof(msg), &opts), RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  p.net.packets.clear();

  Packet bad_ack;
  bad_ack.from = addr(2);
  bad_ack.to = addr(1);
  bad_ack.bytes = make_single_frame_packet(kReliableFlag, /*payload_len=*/4,
                                           /*channel_seq=*/0);
  store_u32_le(bad_ack.bytes, 16, 1);
  bad_ack.bytes.push_back(0xee);
  p.net.packets.push_back(std::move(bad_ack));

  rudp_endpoint_poll(p.ea, p.net.now_ns);
  rudp_status st{};
  rudp_get_status(p.ca, &st);
  EXPECT_EQ(st.inflight_bytes, sizeof(msg));

  std::array<uint8_t, 32> out{};
  size_t len = 0;
  rudp_recv_info info{};
  EXPECT_EQ(rudp_recv(p.ea, out.data(), out.size(), &len, &info), 0);
}

TEST(CoopRudp, SocketPacketLengthAboveCapIsIgnored) {
  Pair p(/*mtu=*/96);
  Packet bad;
  bad.from = addr(1);
  bad.to = addr(2);
  bad.bytes = make_single_frame_packet(kReliableFlag, /*payload_len=*/4,
                                       /*channel_seq=*/0);
  p.net.recv_cap_override_once = bad.bytes.size() - 1u;
  p.net.packets.push_back(std::move(bad));

  rudp_endpoint_poll(p.eb, p.net.now_ns);
  EXPECT_EQ(rudp_endpoint_find_conn(p.eb, 42), nullptr);

  std::array<uint8_t, 32> out{};
  size_t len = 0;
  rudp_recv_info info{};
  EXPECT_EQ(rudp_recv(p.eb, out.data(), out.size(), &len, &info), 0);

  Packet good;
  good.from = addr(1);
  good.to = addr(2);
  good.bytes = make_single_frame_packet(kReliableFlag, /*payload_len=*/4,
                                        /*channel_seq=*/0);
  std::memcpy(good.bytes.data() + kPacketHeaderBytes + kDataFrameHeaderBytes,
              "ok", 3);
  p.net.packets.push_back(std::move(good));
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  ASSERT_EQ(rudp_recv(p.eb, out.data(), out.size(), &len, &info), 1);
  EXPECT_EQ(len, 4u);
  EXPECT_STREQ(reinterpret_cast<char*>(out.data()), "ok");
}

TEST(CoopRudp, AckOnlyDoesNotCreateIncomingConnection) {
  FakeNet net;
  FakeSock sock{&net, addr(2)};
  rudp_endpoint_config cfg = config_for(&sock);
  rudp_endpoint* ep = nullptr;
  ASSERT_EQ(rudp_endpoint_create(&ep, &cfg), 0);

  Packet ack;
  ack.from = addr(1);
  ack.to = addr(2);
  ack.bytes = make_ack_only_packet(42, 1);
  net.packets.push_back(std::move(ack));

  rudp_endpoint_poll(ep, net.now_ns);
  EXPECT_EQ(rudp_endpoint_find_conn(ep, 42), nullptr);
  rudp_endpoint_destroy(ep);
}

TEST(CoopRudp, EmptyDataPacketDoesNotCreateIncomingConnection) {
  Pair p;
  Packet empty;
  empty.from = addr(1);
  empty.to = addr(2);
  empty.bytes = make_empty_data_packet(42, 7);
  p.net.packets.push_back(std::move(empty));

  rudp_endpoint_poll(p.eb, p.net.now_ns);
  EXPECT_EQ(rudp_endpoint_find_conn(p.eb, 42), nullptr);

  std::array<uint8_t, 32> out{};
  size_t len = 0;
  rudp_recv_info info{};
  EXPECT_EQ(rudp_recv(p.eb, out.data(), out.size(), &len, &info), 0);

  Packet good;
  good.from = addr(1);
  good.to = addr(2);
  good.bytes = make_single_frame_packet(kReliableFlag, /*payload_len=*/4,
                                        /*channel_seq=*/0);
  std::memcpy(good.bytes.data() + kPacketHeaderBytes + kDataFrameHeaderBytes,
              "ok", 3);
  p.net.packets.push_back(std::move(good));
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  ASSERT_EQ(rudp_recv(p.eb, out.data(), out.size(), &len, &info), 1);
  EXPECT_EQ(len, 4u);
  EXPECT_STREQ(reinterpret_cast<char*>(out.data()), "ok");
}

TEST(CoopRudp, AckOnlyFromDifferentPeerDoesNotReleaseInflight) {
  Pair p(/*mtu=*/96);
  const char msg[] = "ack-target";
  rudp_send_opts opts{};
  opts.reliability = RUDP_RELIABLE_UNORDERED;
  ASSERT_EQ(rudp_send(p.ca, msg, sizeof(msg), &opts), RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  p.net.packets.clear();

  Packet spoofed_ack;
  spoofed_ack.from = addr(9);
  spoofed_ack.to = addr(1);
  spoofed_ack.bytes = make_ack_only_packet(42, 1);
  p.net.packets.push_back(std::move(spoofed_ack));
  rudp_endpoint_poll(p.ea, p.net.now_ns);

  rudp_status st{};
  rudp_get_status(p.ca, &st);
  EXPECT_EQ(st.inflight_bytes, sizeof(msg));
}

TEST(CoopRudp, AckOnlyWithPacketSequenceDoesNotReleaseInflight) {
  Pair p(/*mtu=*/96);
  const char msg[] = "ack-target";
  rudp_send_opts opts{};
  opts.reliability = RUDP_RELIABLE_UNORDERED;
  ASSERT_EQ(rudp_send(p.ca, msg, sizeof(msg), &opts), RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  p.net.packets.clear();

  Packet bad_ack;
  bad_ack.from = addr(2);
  bad_ack.to = addr(1);
  bad_ack.bytes = make_ack_only_packet(42, 1);
  store_u32_le(bad_ack.bytes, 12, 99);
  p.net.packets.push_back(std::move(bad_ack));
  rudp_endpoint_poll(p.ea, p.net.now_ns);

  rudp_status st{};
  rudp_get_status(p.ca, &st);
  EXPECT_EQ(st.inflight_bytes, sizeof(msg));
}

TEST(CoopRudp, DataFromDifferentPeerDoesNotReleaseInflightOrDeliver) {
  Pair p(/*mtu=*/96);
  const char msg[] = "ack-target";
  rudp_send_opts opts{};
  opts.reliability = RUDP_RELIABLE_UNORDERED;
  ASSERT_EQ(rudp_send(p.ca, msg, sizeof(msg), &opts), RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  p.net.packets.clear();

  Packet spoofed_data;
  spoofed_data.from = addr(9);
  spoofed_data.to = addr(1);
  spoofed_data.bytes = make_single_frame_packet(kReliableFlag,
                                                /*payload_len=*/4,
                                                /*channel_seq=*/0);
  store_u32_le(spoofed_data.bytes, 16, 1);
  p.net.packets.push_back(std::move(spoofed_data));
  rudp_endpoint_poll(p.ea, p.net.now_ns);

  rudp_status st{};
  rudp_get_status(p.ca, &st);
  EXPECT_EQ(st.inflight_bytes, sizeof(msg));

  std::array<uint8_t, 32> out{};
  size_t len = 0;
  rudp_recv_info info{};
  EXPECT_EQ(rudp_recv(p.ea, out.data(), out.size(), &len, &info), 0);
}

TEST(CoopRudp, DataPacketWithZeroSequenceHasNoDeliveryOrConnection) {
  Pair p(/*mtu=*/96);
  Packet bad;
  bad.from = addr(1);
  bad.to = addr(2);
  bad.bytes = make_single_frame_packet(kReliableFlag, /*payload_len=*/4,
                                       /*channel_seq=*/0);
  store_u32_le(bad.bytes, 12, 0);
  p.net.packets.push_back(std::move(bad));

  rudp_endpoint_poll(p.eb, p.net.now_ns);
  EXPECT_EQ(rudp_endpoint_find_conn(p.eb, 42), nullptr);
  std::array<uint8_t, 32> out{};
  size_t len = 0;
  rudp_recv_info info{};
  EXPECT_EQ(rudp_recv(p.eb, out.data(), out.size(), &len, &info), 0);
}

TEST(CoopRudp, InvalidFrameFlagsHaveNoDeliveryOrAck) {
  Pair p(/*mtu=*/96);
  Packet bad;
  bad.from = addr(1);
  bad.to = addr(2);
  bad.bytes = make_single_frame_packet(kOrderedFlag, /*payload_len=*/4,
                                       /*channel_seq=*/0);
  p.net.packets.push_back(std::move(bad));

  rudp_endpoint_poll(p.eb, p.net.now_ns);
  std::array<uint8_t, 32> out{};
  size_t len = 0;
  rudp_recv_info info{};
  EXPECT_EQ(rudp_recv(p.eb, out.data(), out.size(), &len, &info), 0);

  EXPECT_TRUE(p.net.packets.empty());
  rudp_endpoint_flush(p.eb, p.net.now_ns);
  EXPECT_TRUE(p.net.packets.empty());
}

TEST(CoopRudp, ReliableOrderedDrainResumesAfterRecvFreesQueueSpace) {
  Pair p(/*mtu=*/64, /*max_payload=*/0, /*send_batch_size=*/16,
         /*max_recv_events=*/1, /*max_ordered_holds=*/1);
  const char first[] = "one";
  const char second[] = "two";
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

TEST(CoopRudp, BorrowedRecvStaysStableWhenOrderedDrainAddsEvent) {
  Pair p(/*mtu=*/64, /*max_payload=*/0, /*send_batch_size=*/16,
         /*max_recv_events=*/1, /*max_ordered_holds=*/1);
  const char first[] = "one";
  const char second[] = "two";
  rudp_send_opts opts{};
  opts.reliability = RUDP_RELIABLE_ORDERED;
  ASSERT_EQ(rudp_send(p.ca, first, sizeof(first), &opts), RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_EQ(rudp_send(p.ca, second, sizeof(second), &opts), RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);

  ASSERT_GE(p.net.packets.size(), 2u);
  std::swap(p.net.packets[0], p.net.packets[1]);
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  const void* borrowed = nullptr;
  size_t len = 0;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv_borrow(p.eb, &borrowed, &len, &info), 1);
  ASSERT_EQ(len, sizeof(first));
  EXPECT_STREQ(static_cast<const char*>(borrowed), first);

  char out[32]{};
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_STREQ(out, second);
  EXPECT_STREQ(static_cast<const char*>(borrowed), first);
}

TEST(CoopRudp, ReliableOrderedRetransmitsWhenHoldQueueWasFull) {
  Pair p(/*mtu=*/64, /*max_payload=*/0, /*send_batch_size=*/16,
         /*max_recv_events=*/64, /*max_ordered_holds=*/1);
  const char first[] = "one";
  const char second[] = "two";
  const char third[] = "three";
  rudp_send_opts opts{};
  opts.reliability = RUDP_RELIABLE_ORDERED;
  ASSERT_EQ(rudp_send(p.ca, first, sizeof(first), &opts), RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_EQ(rudp_send(p.ca, second, sizeof(second), &opts), RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  ASSERT_EQ(rudp_send(p.ca, third, sizeof(third), &opts), RUDP_SEND_QUEUED);
  rudp_endpoint_flush(p.ea, p.net.now_ns);

  ASSERT_GE(p.net.packets.size(), 3u);
  Packet held_first = std::move(p.net.packets.front());
  p.net.packets.pop_front();
  rudp_endpoint_poll(p.eb, p.net.now_ns);

  p.net.packets.push_back(std::move(held_first));
  rudp_endpoint_poll(p.eb, p.net.now_ns);
  rudp_endpoint_flush(p.eb, p.net.now_ns);
  rudp_endpoint_poll(p.ea, p.net.now_ns);

  char out[32]{};
  size_t len = 0;
  rudp_recv_info info{};
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_STREQ(out, first);
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_STREQ(out, second);
  EXPECT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 0);

  p.net.now_ns += 25'000'000ull;
  p.pump();
  ASSERT_EQ(rudp_recv(p.eb, out, sizeof(out), &len, &info), 1);
  EXPECT_STREQ(out, third);
}

// --- 2026-07-02 全実装レビュー（docs/improvements.md §1）の回帰テスト ---

// §1.1: abort で conn_map からエントリが消え、スロット再利用後も古い conn_id の
// lookup が別コネクションを返さない。tombstone 枯れで insert が失敗しないこと
// も接続チャーンで確認する（conn_map_cap = max_conns*4 = 32 を大きく超える回数）。
TEST(CoopRudp, ConnMapSurvivesConnectionChurnAndSlotReuse) {
  FakeNet net;
  FakeSock sock{&net, addr(1)};
  rudp_endpoint_config cfg = config_for(&sock);
  rudp_endpoint* ep = nullptr;
  ASSERT_EQ(rudp_endpoint_create(&ep, &cfg), 0);
  auto peer = addr(2);

  uint64_t prev_id = 0;
  for (uint64_t i = 1; i <= 200; ++i) {
    uint64_t id = 1000 + i;
    rudp_conn* c = nullptr;
    ASSERT_EQ(rudp_endpoint_connect(ep, &peer, id, &c), 0) << "churn " << i;
    ASSERT_NE(c, nullptr);
    if (prev_id != 0) {
      // abort 済みの旧 conn_id はスロットが再利用されても引けない。
      EXPECT_EQ(rudp_endpoint_find_conn(ep, prev_id), nullptr) << "churn " << i;
    }
    EXPECT_EQ(rudp_endpoint_find_conn(ep, id), c);
    rudp_conn_abort(c);
    EXPECT_EQ(rudp_endpoint_find_conn(ep, id), nullptr);
    prev_id = id;
  }
  rudp_endpoint_destroy(ep);
}

// §1.6: reliable の in-flight は SACK bitmap 幅（64）を超えて発行されない。
// ACK が一切返らない状態で flush を繰り返しても、ワイヤに出る reliable
// パケットは 64 個で止まる（65 個目以降は SACK 不能で不要再送になるため）。
TEST(CoopRudp, ReliableInflightGateStopsAtAckWindow) {
  Pair p(/*mtu=*/64);  // 1 パケット 1 メッセージになるサイズ。
  std::array<uint8_t, 12> msg{};
  rudp_send_opts opts{};
  opts.reliability = RUDP_RELIABLE_UNORDERED;
  for (int i = 0; i < 100; ++i) {
    msg.fill(static_cast<uint8_t>(i));
    ASSERT_EQ(rudp_send(p.ca, msg.data(), msg.size(), &opts), RUDP_SEND_QUEUED);
  }
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  EXPECT_EQ(p.net.packets.size(), 64u);
  // ACK なしで再度 flush しても新しい seq は発行されない。
  rudp_endpoint_flush(p.ea, p.net.now_ns);
  EXPECT_EQ(p.net.packets.size(), 64u);
}

// §1.3: pacing_bps が有限のとき flush はトークンで送出を絞り、時間経過で
// 続きが送出される（safe_bps/pacing_bps が status 表示専用でないこと）。
TEST(CoopRudp, PacingGatesFlushWhenRateIsFinite) {
  FakeNet net;
  FakeSock a{&net, addr(1)};
  rudp_endpoint_config ca_cfg = config_for(&a);
  ca_cfg.max_messages = 1024;
  ca_cfg.initial_safe_bps = 800'000;  // 100KB/s。バースト上限 16*mtu=8KB。
  rudp_endpoint* ea = nullptr;
  ASSERT_EQ(rudp_endpoint_create(&ea, &ca_cfg), 0);
  auto baddr = addr(2);
  rudp_conn* ca = nullptr;
  ASSERT_EQ(rudp_endpoint_connect(ea, &baddr, 42, &ca), 0);

  std::array<uint8_t, 12> msg{};
  rudp_send_opts opts{};
  opts.reliability = RUDP_UNRELIABLE;
  for (int i = 0; i < 600; ++i) {
    msg.fill(static_cast<uint8_t>(i));
    ASSERT_EQ(rudp_send(ca, msg.data(), msg.size(), &opts), RUDP_SEND_QUEUED);
  }
  rudp_endpoint_flush(ea, net.now_ns);
  rudp_status st{};
  rudp_get_status(ca, &st);
  // バースト上限（16*mtu=8KB）までしか送れず、残りはキューに留まる。
  EXPECT_GT(st.send_queue_bytes, 0u);

  // 時間を進めながら flush すればトークンが補充されて掃ける
  // （1 回の flush で送れるのはバースト上限まで）。
  for (int i = 0; i < 5 && st.send_queue_bytes > 0; ++i) {
    net.now_ns += 1'000'000'000ull;
    rudp_endpoint_flush(ea, net.now_ns);
    rudp_get_status(ca, &st);
  }
  EXPECT_EQ(st.send_queue_bytes, 0u);
  rudp_endpoint_destroy(ea);
}

// §1.4: ordered の channel_seq はキュー投入が成功するまで消費されない。
// msg pool 枯渇で送信が失敗しても seq に穴が空かず、その後の送信が受信側で
// 順番どおり全部届く（穴空きだと expected_ordered が永久に待つ）。
TEST(CoopRudp, OrderedSeqNotConsumedByFailedSend) {
  Pair p(/*mtu=*/64);
  std::array<uint8_t, 12> msg{};
  rudp_send_opts opts{};
  opts.reliability = RUDP_RELIABLE_ORDERED;

  // max_messages=128 を使い切るまで送って必ず失敗を踏む。
  int queued = 0;
  for (int i = 0; i < 200; ++i) {
    msg.fill(static_cast<uint8_t>(i));
    rudp_send_result r = rudp_send(p.ca, msg.data(), msg.size(), &opts);
    if (r == RUDP_SEND_QUEUED) {
      ++queued;
    } else {
      ASSERT_EQ(r, RUDP_SEND_QUEUE_FULL);
      break;
    }
  }
  ASSERT_GT(queued, 0);

  // pump と受信ドレインを交互に行う（受信イベントキューは 64 しかないので
  // ドレインしないと reliable の再送ループになる）。
  std::array<uint8_t, 12> out{};
  size_t len = 0;
  rudp_recv_info info{};
  int received = 0;
  uint8_t last = 0;
  auto drain = [&]() {
    while (rudp_recv(p.eb, out.data(), out.size(), &len, &info) == 1) {
      ++received;
      last = out[0];
    }
  };
  for (int round = 0; round < 20; ++round) {
    p.pump();
    drain();
  }
  // 掃けてから追加送信。
  msg.fill(0xAA);
  ASSERT_EQ(rudp_send(p.ca, msg.data(), msg.size(), &opts), RUDP_SEND_QUEUED);
  ++queued;
  for (int round = 0; round < 20; ++round) {
    p.pump();
    drain();
  }
  // 失敗 send が seq を消費していると、失敗以降の ordered が hold されたまま
  // received が queued に届かない。
  EXPECT_EQ(received, queued);
  EXPECT_EQ(last, 0xAA);  // 最後に受けたのは追加送信分。
}

// §1.6: max_incoming_conns_per_poll が受信パケット起点の conn 生成を
// 1 poll あたりで制限する（spoof された conn_id での conn テーブル枯渇の緩和）。
TEST(CoopRudp, IncomingConnCreationIsRateLimitedPerPoll) {
  FakeNet net;
  FakeSock sock{&net, addr(2)};
  rudp_endpoint_config cfg = config_for(&sock);
  cfg.max_incoming_conns_per_poll = 1;
  rudp_endpoint* ep = nullptr;
  ASSERT_EQ(rudp_endpoint_create(&ep, &cfg), 0);

  auto push_packet_for = [&](uint64_t conn_id) {
    Packet pk;
    pk.from = addr(1);
    pk.to = addr(2);
    pk.bytes = make_single_frame_packet(/*flags=*/0, /*payload_len=*/4,
                                        /*channel_seq=*/0);
    store_u64_le(pk.bytes, 4, conn_id);
    net.packets.push_back(std::move(pk));
  };

  push_packet_for(101);
  push_packet_for(102);
  rudp_endpoint_poll(ep, net.now_ns);
  EXPECT_NE(rudp_endpoint_find_conn(ep, 101), nullptr);
  EXPECT_EQ(rudp_endpoint_find_conn(ep, 102), nullptr);

  // 次の poll では新たに 1 つ生成できる。
  push_packet_for(102);
  rudp_endpoint_poll(ep, net.now_ns);
  EXPECT_NE(rudp_endpoint_find_conn(ep, 102), nullptr);
  rudp_endpoint_destroy(ep);
}

// §16: adapter が使う precheck API がコアの受理条件と整合している。
TEST(CoopRudp, PacketPrecheckMatchesCoreAcceptance) {
  auto bytes = make_single_frame_packet(/*flags=*/0, /*payload_len=*/4,
                                        /*channel_seq=*/0);
  uint64_t conn_id = 0;
  EXPECT_EQ(rudp_packet_precheck(bytes.data(), bytes.size(), 512, &conn_id), 1);
  EXPECT_EQ(conn_id, 42u);
  // magic 破壊で不合格。
  auto broken = bytes;
  broken[3] ^= 0xff;
  EXPECT_EQ(rudp_packet_precheck(broken.data(), broken.size(), 512, nullptr), 0);
  // ヘッダ未満の長さは不合格。
  EXPECT_EQ(rudp_packet_precheck(bytes.data(), 31, 512, nullptr), 0);
}
