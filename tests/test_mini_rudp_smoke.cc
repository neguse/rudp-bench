#include <gtest/gtest.h>
#include "harness/adapter_registry.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rudp_bench { void register_mini_rudp_adapter(); }

class MiniRudpRegistrar {
 public:
  MiniRudpRegistrar() { rudp_bench::register_mini_rudp_adapter(); }
};
static MiniRudpRegistrar registrar;

using namespace rudp_bench;

namespace {

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

}  // namespace

TEST(MiniRudpSmoke, ReliableEcho) {
  auto server = create_adapter("mini_rudp");
  auto client = create_adapter("mini_rudp");
  ASSERT_NE(server, nullptr);
  server->server_listen(0xC101);

  std::thread server_thread([&]() {
    char buf[2048]; size_t len; uint32_t cid;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      server->poll();
      if (server->recv(buf, sizeof(buf), &len, &cid) == 1) {
        server->send(cid, buf, len, true);
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  uint32_t cid = client->client_connect("127.0.0.1", 0xC101);
  const char msg[] = "rudp-hello";
  EXPECT_EQ(client->send(cid, msg, sizeof(msg), true), 0);

  char buf[2048]; size_t len; uint32_t in_cid;
  bool got = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    client->poll();
    if (client->recv(buf, sizeof(buf), &len, &in_cid) == 1) {
      EXPECT_EQ(len, sizeof(msg));
      EXPECT_STREQ(buf, msg);
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

TEST(MiniRudpSmoke, LargePayloadEchoDoesNotTruncate) {
  auto server = create_adapter("mini_rudp");
  auto client = create_adapter("mini_rudp");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);
  server->server_listen(0xC102);

  std::thread server_thread([&]() {
    std::vector<uint8_t> buf(4096);
    size_t len;
    uint32_t cid;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      server->poll();
      if (server->recv(buf.data(), buf.size(), &len, &cid) == 1) {
        server->send(cid, buf.data(), len, true);
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  uint32_t cid = client->client_connect("127.0.0.1", 0xC102);
  std::vector<uint8_t> msg(3000);
  for (size_t i = 0; i < msg.size(); ++i) msg[i] = static_cast<uint8_t>(i);
  EXPECT_EQ(client->send(cid, msg.data(), msg.size(), true), 0);

  std::vector<uint8_t> buf(4096);
  size_t len;
  uint32_t in_cid;
  bool got = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    client->poll();
    if (client->recv(buf.data(), buf.size(), &len, &in_cid) == 1) {
      EXPECT_EQ(len, msg.size());
      EXPECT_EQ(std::memcmp(buf.data(), msg.data(), msg.size()), 0);
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

TEST(MiniRudpSmoke, Capability) {
  auto a = create_adapter("mini_rudp");
  EXPECT_TRUE(a->supports(true));
  EXPECT_TRUE(a->supports(false));
  // 10-byte wire header (flags u16 + conv u32 + seq u32) after L11 multiplexing.
  EXPECT_EQ(a->max_payload_bytes(true), 65497u);
  EXPECT_EQ(a->max_payload_bytes(false), 65497u);
  EXPECT_FALSE(a->encryption_on());
  EXPECT_STREQ(a->name(), "mini_rudp");
}

TEST(MiniRudpSmoke, ReliableSendBackpressuresWhenPendingQueueIsFull) {
  auto client = create_adapter("mini_rudp");
  ASSERT_NE(client, nullptr);
  uint32_t cid = client->client_connect("127.0.0.1", 0xC1FE);

  const char msg[] = "x";
  size_t accepted = 0;
  while (client->send(cid, msg, sizeof(msg), true) == 0) {
    ++accepted;
  }

  EXPECT_EQ(accepted, 65536u);
  client->close();
}

TEST(MiniRudpSmoke, ReliableSendTimesOutDisconnectedPeer) {
  ScopedEnv timeout("MINI_RUDP_RELIABLE_TIMEOUT_MS", "20");
  auto client = create_adapter("mini_rudp");
  ASSERT_NE(client, nullptr);

  uint32_t cid = client->client_connect("127.0.0.1", 0xC1FD);
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

TEST(MiniRudpSmoke, ReliableEchoManyConnectionsPreservesClientConnIds) {
  constexpr size_t kConns = 128;
  constexpr uint16_t kPort = 0xC103;
  constexpr uint32_t kMarker = 0xCCDDEEFFu;

  auto server = create_adapter("mini_rudp");
  auto client = create_adapter("mini_rudp");
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
