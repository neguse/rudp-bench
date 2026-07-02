#include <gtest/gtest.h>

#include "harness/adapter_registry.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rudp_bench { void register_apex_rudp_adapter(); }

class ApexRudpRegistrar {
 public:
  ApexRudpRegistrar() { rudp_bench::register_apex_rudp_adapter(); }
};
static ApexRudpRegistrar registrar;

using namespace rudp_bench;

namespace {

constexpr uint8_t kFlagReliable = 0x01;
constexpr uint8_t kFlagAckOnly = 0x02;
constexpr uint8_t kFlagHasAck = 0x04;
constexpr uint8_t kFlagBatch = 0x08;
constexpr size_t kApexHeaderBytes = 1 + 4 + 4 + 4 + 8;

class ScopedEnv {
 public:
  ScopedEnv(const char* name, const char* value) : name_(name) {
    const char* old = std::getenv(name);
    if (old) {
      had_old_ = true;
      old_ = old;
    }
    ::setenv(name_.c_str(), value, 1);
  }

  ~ScopedEnv() {
    if (had_old_) {
      ::setenv(name_.c_str(), old_.c_str(), 1);
    } else {
      ::unsetenv(name_.c_str());
    }
  }

 private:
  std::string name_;
  bool had_old_ = false;
  std::string old_;
};

void store_u32_le(std::vector<uint8_t>& bytes, size_t off, uint32_t v) {
  bytes[off + 0] = static_cast<uint8_t>(v & 0xffu);
  bytes[off + 1] = static_cast<uint8_t>((v >> 8) & 0xffu);
  bytes[off + 2] = static_cast<uint8_t>((v >> 16) & 0xffu);
  bytes[off + 3] = static_cast<uint8_t>((v >> 24) & 0xffu);
}

std::vector<uint8_t> make_raw_apex_packet(uint32_t seq, const char* payload,
                                          uint32_t conv) {
  size_t payload_len = std::strlen(payload) + 1;
  std::vector<uint8_t> bytes(kApexHeaderBytes + payload_len);
  bytes[0] = kFlagReliable;
  store_u32_le(bytes, 1, conv);
  store_u32_le(bytes, 5, seq);
  store_u32_le(bytes, 9, 0);      // ack
  store_u32_le(bytes, 13, 0);     // ack_bits low
  store_u32_le(bytes, 17, 0);     // ack_bits high
  std::memcpy(bytes.data() + kApexHeaderBytes, payload, payload_len);
  return bytes;
}

void send_raw_bytes(int fd, uint16_t port, const std::vector<uint8_t>& bytes) {
  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(port);
  ASSERT_EQ(inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr), 1);
  ASSERT_EQ(::sendto(fd, bytes.data(), bytes.size(), 0,
                     reinterpret_cast<sockaddr*>(&dst), sizeof(dst)),
            static_cast<ssize_t>(bytes.size()));
}

void send_raw_apex_packet(int fd, uint16_t port, uint32_t seq,
                          const char* payload, uint32_t conv = 1) {
  send_raw_bytes(fd, port, make_raw_apex_packet(seq, payload, conv));
}

// datagram (batch フレーム含む) から reliable パケットの seq を収集する
template <typename SeqContainer>
void collect_reliable_seqs(const uint8_t* p, size_t n, SeqContainer& seqs) {
  if (n < kApexHeaderBytes) return;
  uint8_t flags = p[0];
  if ((flags & kFlagBatch) != 0) {
    size_t off = kApexHeaderBytes;
    while (off + 2 <= n) {
      uint16_t frame_len;
      std::memcpy(&frame_len, p + off, 2);
      off += 2;
      if (off + frame_len > n) return;
      collect_reliable_seqs(p + off, frame_len, seqs);
      off += frame_len;
    }
    return;
  }
  if ((flags & kFlagReliable) != 0) {
    uint32_t seq;
    std::memcpy(&seq, p + 5, 4);
    seqs.insert(seq);
  }
}

template <typename SeqContainer>
void drain_raw_socket(int fd, SeqContainer& seqs, sockaddr_in* client_addr) {
  uint8_t buf[65536];
  while (true) {
    sockaddr_in src{};
    socklen_t src_len = sizeof(src);
    ssize_t n = ::recvfrom(fd, buf, sizeof(buf), MSG_DONTWAIT,
                           reinterpret_cast<sockaddr*>(&src), &src_len);
    if (n < 0) break;
    if (client_addr) *client_addr = src;
    collect_reliable_seqs(buf, static_cast<size_t>(n), seqs);
  }
}

void send_raw_ack_packet(int fd, const sockaddr_in& dst, uint32_t conv,
                         uint32_t ack, uint64_t ack_bits) {
  std::vector<uint8_t> bytes(kApexHeaderBytes, 0);
  bytes[0] = kFlagAckOnly | kFlagHasAck;
  store_u32_le(bytes, 1, conv);
  store_u32_le(bytes, 5, 0);  // seq
  store_u32_le(bytes, 9, ack);
  store_u32_le(bytes, 13, static_cast<uint32_t>(ack_bits & 0xFFFFFFFFu));
  store_u32_le(bytes, 17, static_cast<uint32_t>(ack_bits >> 32));
  ASSERT_EQ(::sendto(fd, bytes.data(), bytes.size(), 0,
                     reinterpret_cast<const sockaddr*>(&dst), sizeof(dst)),
            static_cast<ssize_t>(bytes.size()));
}

}  // namespace

TEST(ApexRudpSmoke, ReliableEcho) {
  auto server = create_adapter("apex_rudp");
  auto client = create_adapter("apex_rudp");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);
  server->server_listen(0xC201);

  std::thread server_thread([&]() {
    char buf[2048];
    size_t len;
    uint32_t cid;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      server->poll();
      if (server->recv(buf, sizeof(buf), &len, &cid) == 1) {
        server->send(cid, buf, len, true);
        server->poll();
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  uint32_t cid = client->client_connect("127.0.0.1", 0xC201);
  const char msg[] = "apex-hello";
  EXPECT_EQ(client->send(cid, msg, sizeof(msg), true), 0);

  char buf[2048];
  size_t len;
  uint32_t in_cid;
  bool got = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    client->poll();
    if (client->recv(buf, sizeof(buf), &len, &in_cid) == 1) {
      EXPECT_EQ(len, sizeof(msg));
      EXPECT_STREQ(buf, msg);
      EXPECT_EQ(in_cid, cid);
      got = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  EXPECT_TRUE(got);
  server_thread.join();
  client->close();
  server->close();
}

TEST(ApexRudpSmoke, UnreliableEcho) {
  auto server = create_adapter("apex_rudp");
  auto client = create_adapter("apex_rudp");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);
  server->server_listen(0xC202);

  std::thread server_thread([&]() {
    char buf[2048];
    size_t len;
    uint32_t cid;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      server->poll();
      if (server->recv(buf, sizeof(buf), &len, &cid) == 1) {
        server->send(cid, buf, len, false);
        server->poll();
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  uint32_t cid = client->client_connect("127.0.0.1", 0xC202);
  const char msg[] = "apex-u";
  EXPECT_EQ(client->send(cid, msg, sizeof(msg), false), 0);

  char buf[2048];
  size_t len;
  uint32_t in_cid;
  bool got = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    client->poll();
    if (client->recv(buf, sizeof(buf), &len, &in_cid) == 1) {
      EXPECT_EQ(len, sizeof(msg));
      EXPECT_STREQ(buf, msg);
      EXPECT_EQ(in_cid, cid);
      got = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  EXPECT_TRUE(got);
  server_thread.join();
  client->close();
  server->close();
}

TEST(ApexRudpSmoke, Capability) {
  auto a = create_adapter("apex_rudp");
  ASSERT_NE(a, nullptr);
  EXPECT_TRUE(a->supports(true));
  EXPECT_TRUE(a->supports(false));
  EXPECT_EQ(a->max_payload_bytes(true), 65486u);
  EXPECT_EQ(a->max_payload_bytes(false), 65486u);
  EXPECT_FALSE(a->encryption_on());
  EXPECT_STREQ(a->name(), "apex_rudp");
}

TEST(ApexRudpSmoke, ReliableSendBackpressuresWhenPendingQueueIsFull) {
  auto server = create_adapter("apex_rudp");
  auto client = create_adapter("apex_rudp");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);
  server->server_listen(0xC2FE);
  uint32_t cid = client->client_connect("127.0.0.1", 0xC2FE);

  const char msg[] = "x";
  size_t accepted = 0;
  while (client->send(cid, msg, sizeof(msg), true) == 0) {
    ++accepted;
  }

  EXPECT_EQ(accepted, 4096u);
  client->close();
  server->close();
}

TEST(ApexRudpSmoke, ReliableSendTimesOutDisconnectedPeer) {
  ScopedEnv timeout("APEX_RELIABLE_TIMEOUT_MS", "20");
  auto client = create_adapter("apex_rudp");
  ASSERT_NE(client, nullptr);

  uint32_t cid = client->client_connect("127.0.0.1", 0xC2FD);
  const char msg[] = "timeout";
  ASSERT_TRUE(client->is_connected(cid));
  ASSERT_EQ(client->send(cid, msg, sizeof(msg), true), 0);

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (std::chrono::steady_clock::now() < deadline && client->is_connected(cid)) {
    client->poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  EXPECT_FALSE(client->is_connected(cid));
  EXPECT_EQ(client->send(cid, msg, sizeof(msg), true), -1);
  EXPECT_EQ(client->connection_stats().shutdown_by_transport, 1u);
  client->close();
}

TEST(ApexRudpSmoke, ReliableSeqWrapDeliversPostWrapPacket) {
  constexpr uint16_t kPort = 0xC2FC;
  auto server = create_adapter("apex_rudp");
  ASSERT_NE(server, nullptr);
  server->server_listen(kPort);

  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(fd, 0);
  send_raw_apex_packet(fd, kPort, 0xFFFF'FFFEu, "before-wrap");
  send_raw_apex_packet(fd, kPort, 0xFFFF'FFFFu, "at-wrap");
  send_raw_apex_packet(fd, kPort, 1u, "after-wrap");

  std::vector<std::string> got;
  char buf[64]{};
  size_t len = 0;
  uint32_t cid = 0;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (got.size() < 3 && std::chrono::steady_clock::now() < deadline) {
    server->poll();
    while (server->recv(buf, sizeof(buf), &len, &cid) == 1) {
      got.emplace_back(buf, len > 0 ? len - 1 : 0);
    }
    if (got.size() < 3) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  EXPECT_EQ(got.size(), 3u);
  if (got.size() == 3u) {
    EXPECT_EQ(got[0], "before-wrap");
    EXPECT_EQ(got[1], "at-wrap");
    EXPECT_EQ(got[2], "after-wrap");
  }
  ::close(fd);
  server->close();
}

TEST(ApexRudpSmoke, ReliableEchoManyConnectionsPreservesClientConnIds) {
  constexpr size_t kConns = 128;
  constexpr uint16_t kPort = 0xC203;
  constexpr uint32_t kMarker = 0xAABBCCDDu;

  auto server = create_adapter("apex_rudp");
  auto client = create_adapter("apex_rudp");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);

  server->server_listen(kPort);
  std::atomic<bool> client_done{false};

  std::thread server_thread([&]() {
    uint32_t buf[2];
    size_t len;
    uint32_t cid;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!client_done.load() && std::chrono::steady_clock::now() < deadline) {
      server->poll();
      int r = server->recv(buf, sizeof(buf), &len, &cid);
      if (r == 1) {
        server->send(cid, buf, len, true);
        continue;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  std::vector<uint32_t> conn_ids;
  std::unordered_map<uint32_t, uint32_t> index_by_conn;
  conn_ids.reserve(kConns);
  index_by_conn.reserve(kConns);
  for (uint32_t i = 0; i < kConns; ++i) {
    uint32_t cid = client->client_connect("127.0.0.1", kPort);
    conn_ids.push_back(cid);
    index_by_conn[cid] = i;
  }

  for (uint32_t i = 0; i < kConns; ++i) {
    uint32_t payload[2] = {kMarker, i};
    ASSERT_EQ(client->send(conn_ids[i], payload, sizeof(payload), true), 0);
  }

  std::vector<bool> got(kConns, false);
  size_t received = 0;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (received < kConns && std::chrono::steady_clock::now() < deadline) {
    client->poll();
    for (;;) {
      uint32_t payload[2] = {};
      size_t len = 0;
      uint32_t in_cid = 0;
      int r = client->recv(payload, sizeof(payload), &len, &in_cid);
      if (r == 0) break;
      ASSERT_EQ(r, 1);
      ASSERT_EQ(len, sizeof(payload));
      ASSERT_EQ(payload[0], kMarker);

      auto it = index_by_conn.find(in_cid);
      ASSERT_NE(it, index_by_conn.end());
      uint32_t expected_index = it->second;
      EXPECT_EQ(payload[1], expected_index);
      if (!got[expected_index]) {
        got[expected_index] = true;
        ++received;
      }
    }
    if (received < kConns) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  client_done.store(true);
  EXPECT_EQ(received, kConns);

  server_thread.join();
  client->close();
  server->close();
}

// §2.1 回帰: リモート任意指定の conv を vector index に使って巨大アロケーション
// (conv=0xFFFFFFFF で ~16GB) しないこと。map 化後は普通に配送されるだけ。
TEST(ApexRudpSmoke, HugeConvDoesNotTriggerHugeAllocation) {
  constexpr uint16_t kPort = 0xC2E1;
  auto server = create_adapter("apex_rudp");
  ASSERT_NE(server, nullptr);
  server->server_listen(kPort);

  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(fd, 0);
  send_raw_apex_packet(fd, kPort, 1u, "huge-conv", 0xFFFF'FFFFu);
  send_raw_apex_packet(fd, kPort, 1u, "mid-conv", 0x7FFF'FFFFu);

  std::set<std::string> got;
  char buf[64]{};
  size_t len = 0;
  uint32_t cid = 0;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (got.size() < 2 && std::chrono::steady_clock::now() < deadline) {
    server->poll();
    while (server->recv(buf, sizeof(buf), &len, &cid) == 1) {
      got.emplace(buf, len > 0 ? len - 1 : 0);
    }
    if (got.size() < 2) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  EXPECT_TRUE(got.count("huge-conv"));
  EXPECT_TRUE(got.count("mid-conv"));
  ::close(fd);
  server->close();
}

// §2.2 回帰: 送信側の in-flight 窓は受信 SACK 窓 (64bit bitmap) と一致すること。
// 200 件送っても未ACKのままではワイヤに 64 seq しか出ず、ACK で窓が進むと
// 続き (65..128) が送出される。
TEST(ApexRudpWindow, InFlightWindowMatchesSackWidthAndAdvancesOnAck) {
  constexpr uint16_t kPort = 0xC2F0;
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(fd, 0);
  sockaddr_in bind_addr{};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_port = htons(kPort);
  ASSERT_EQ(inet_pton(AF_INET, "127.0.0.1", &bind_addr.sin_addr), 1);
  ASSERT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&bind_addr),
                   sizeof(bind_addr)),
            0);

  auto client = create_adapter("apex_rudp");
  ASSERT_NE(client, nullptr);
  uint32_t cid = client->client_connect("127.0.0.1", kPort);

  const char msg[] = "w";
  for (int i = 0; i < 200; ++i) {
    ASSERT_EQ(client->send(cid, msg, sizeof(msg), true), 0);
  }

  // ACK なしで 250ms 観測: 再送を含めても distinct seq は 1..64 のみのはず
  std::set<uint32_t> seqs;
  sockaddr_in client_addr{};
  auto observe_until =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
  while (std::chrono::steady_clock::now() < observe_until) {
    client->poll();
    drain_raw_socket(fd, seqs, &client_addr);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_FALSE(seqs.empty());
  EXPECT_EQ(*seqs.begin(), 1u);
  EXPECT_EQ(*seqs.rbegin(), 64u);
  EXPECT_EQ(seqs.size(), 64u);

  // seq 1..64 を全て ACK → 窓が進んで 65..128 が送出される
  send_raw_ack_packet(fd, client_addr, cid, 64u, ~uint64_t{0});
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (*seqs.rbegin() < 128u &&
         std::chrono::steady_clock::now() < deadline) {
    client->poll();
    drain_raw_socket(fd, seqs, nullptr);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(*seqs.rbegin(), 128u);
  for (uint32_t s = 65; s <= 128; ++s) {
    EXPECT_TRUE(seqs.count(s)) << "missing seq " << s;
  }

  client->close();
  ::close(fd);
}

// §2.3 回帰: SACK の穴 (先の seq だけ ACK されている状態) を 3 回観測したら、
// RTO タイマを待たずに fast retransmit すること。
// タイマ再送と区別するため、全パケットを一度タイマ再送させて retx_count>=1 に
// してから穴あき ACK を送る (Karn により RTT サンプルが取られず RTO は 100ms の
// まま、かつ seq1 の次のタイマ再送はバックオフで +200ms 先になる)。
TEST(ApexRudpWindow, FastRetransmitFillsSackHole) {
  constexpr uint16_t kPort = 0xC2E7;
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(fd, 0);
  sockaddr_in bind_addr{};
  bind_addr.sin_family = AF_INET;
  bind_addr.sin_port = htons(kPort);
  ASSERT_EQ(inet_pton(AF_INET, "127.0.0.1", &bind_addr.sin_addr), 1);
  ASSERT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&bind_addr),
                   sizeof(bind_addr)),
            0);

  auto client = create_adapter("apex_rudp");
  ASSERT_NE(client, nullptr);
  uint32_t cid = client->client_connect("127.0.0.1", kPort);
  const char msg[] = "f";
  for (int i = 0; i < 5; ++i) {
    ASSERT_EQ(client->send(cid, msg, sizeof(msg), true), 0);
  }

  // 初回送出 + 1 回目のタイマ再送 (~100ms) まで観測
  std::multiset<uint32_t> seqs;
  sockaddr_in client_addr{};
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (seqs.count(1u) < 2 && std::chrono::steady_clock::now() < deadline) {
    client->poll();
    drain_raw_socket(fd, seqs, &client_addr);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_GE(seqs.count(1u), 2u);  // 初回 + タイマ再送

  size_t seq1_before = seqs.count(1u);
  // seq 2..5 のみ ACK (seq1 が SACK の穴) × 3 datagram
  // ack=5, bits: delta 0..3 (seq 5..2) セット, delta 4 (seq 1) 未セット
  for (int i = 0; i < 3; ++i) {
    send_raw_ack_packet(fd, client_addr, cid, 5u, 0x0Fu);
  }

  // fast retransmit は次のタイマ (バックオフで +200ms 先) を待たず数 ms で出る
  bool retransmitted = false;
  deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
  while (std::chrono::steady_clock::now() < deadline) {
    client->poll();
    drain_raw_socket(fd, seqs, nullptr);
    if (seqs.count(1u) > seq1_before) {
      retransmitted = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(retransmitted);

  client->close();
  ::close(fd);
}

// §2.3 回帰: 任意 src からの無制限 Conn 生成を上限で破棄すること
TEST(ApexRudpSmoke, ConnectionLimitDropsExcessPeers) {
  ScopedEnv limit("APEX_MAX_CONNECTIONS", "2");
  constexpr uint16_t kPort = 0xC2E3;
  auto server = create_adapter("apex_rudp");
  ASSERT_NE(server, nullptr);
  EXPECT_EQ(server->max_connections(), 2u);
  server->server_listen(kPort);

  int fds[3];
  for (int i = 0; i < 3; ++i) {
    fds[i] = ::socket(AF_INET, SOCK_DGRAM, 0);
    ASSERT_GE(fds[i], 0);
  }
  send_raw_apex_packet(fds[0], kPort, 1u, "peer-0");
  send_raw_apex_packet(fds[1], kPort, 1u, "peer-1");
  send_raw_apex_packet(fds[2], kPort, 1u, "peer-2");

  std::set<uint32_t> cids;
  size_t delivered = 0;
  char buf[64]{};
  size_t len = 0;
  uint32_t cid = 0;
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
  while (std::chrono::steady_clock::now() < deadline) {
    server->poll();
    while (server->recv(buf, sizeof(buf), &len, &cid) == 1) {
      cids.insert(cid);
      ++delivered;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // 3 peer のうち 2 つ分だけ Conn が作られ、3 つ目は破棄される
  EXPECT_EQ(delivered, 2u);
  EXPECT_EQ(cids.size(), 2u);
  EXPECT_EQ(server->connection_stats().connected_peak, 2u);

  for (int i = 0; i < 3; ++i) ::close(fds[i]);
  server->close();
}

TEST(ApexRudpSmoke, MaxConnectionsFollowsHint) {
  auto a = create_adapter("apex_rudp");
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->max_connections(), 4096u);  // hint も env も無い時の既定値
  a->hint_connections(1000);
  EXPECT_EQ(a->max_connections(), 1000u + 1000u / 8 + 64u);
}

// batch フレームのネスト (フレーム内 FLAG_BATCH) は破棄されること
TEST(ApexRudpSmoke, NestedBatchFrameIsRejected) {
  constexpr uint16_t kPort = 0xC2E5;
  auto server = create_adapter("apex_rudp");
  ASSERT_NE(server, nullptr);
  server->server_listen(kPort);

  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(fd, 0);

  auto wrap_in_batch = [](const std::vector<uint8_t>& frame) {
    std::vector<uint8_t> batch(kApexHeaderBytes, 0);
    batch[0] = kFlagBatch;
    uint16_t frame_len = static_cast<uint16_t>(frame.size());
    batch.push_back(static_cast<uint8_t>(frame_len & 0xffu));
    batch.push_back(static_cast<uint8_t>(frame_len >> 8));
    batch.insert(batch.end(), frame.begin(), frame.end());
    return batch;
  };

  std::vector<uint8_t> rel = make_raw_apex_packet(1u, "nested", 1u);
  std::vector<uint8_t> batch1 = wrap_in_batch(rel);
  std::vector<uint8_t> batch2 = wrap_in_batch(batch1);  // 不正なネスト

  send_raw_bytes(fd, kPort, batch2);
  char buf[64]{};
  size_t len = 0;
  uint32_t cid = 0;
  auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
  bool got = false;
  while (std::chrono::steady_clock::now() < deadline) {
    server->poll();
    if (server->recv(buf, sizeof(buf), &len, &cid) == 1) {
      got = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_FALSE(got) << "nested batch frame should be dropped";

  // 正常な 1 段 batch は配送される (parser 自体の sanity)
  send_raw_bytes(fd, kPort, batch1);
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!got && std::chrono::steady_clock::now() < deadline) {
    server->poll();
    if (server->recv(buf, sizeof(buf), &len, &cid) == 1) {
      EXPECT_STREQ(buf, "nested");
      got = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(got);

  ::close(fd);
  server->close();
}
