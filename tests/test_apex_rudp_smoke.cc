#include <gtest/gtest.h>

#include "harness/adapter_registry.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
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

void send_raw_apex_packet(int fd, uint16_t port, uint32_t seq,
                          const char* payload) {
  size_t payload_len = std::strlen(payload) + 1;
  std::vector<uint8_t> bytes(kApexHeaderBytes + payload_len);
  bytes[0] = kFlagReliable;
  store_u32_le(bytes, 1, 1);      // conv
  store_u32_le(bytes, 5, seq);
  store_u32_le(bytes, 9, 0);      // ack
  store_u32_le(bytes, 13, 0);     // ack_bits low
  store_u32_le(bytes, 17, 0);     // ack_bits high
  std::memcpy(bytes.data() + kApexHeaderBytes, payload, payload_len);

  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(port);
  ASSERT_EQ(inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr), 1);
  ASSERT_EQ(::sendto(fd, bytes.data(), bytes.size(), 0,
                     reinterpret_cast<sockaddr*>(&dst), sizeof(dst)),
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
