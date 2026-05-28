#include <gtest/gtest.h>
#include "harness/adapter_registry.h"

#include <atomic>
#include <thread>
#include <chrono>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace rudp_bench { void register_raw_udp_adapter(); }

class RawUdpRegistrar {
 public:
  RawUdpRegistrar() { rudp_bench::register_raw_udp_adapter(); }
};
static RawUdpRegistrar registrar;

using namespace rudp_bench;

TEST(RawUdpSmoke, EchoOneMessage) {
  auto server = create_adapter("raw_udp");
  auto client = create_adapter("raw_udp");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);

  server->server_listen(0xC100);

  std::atomic<bool> done{false};
  std::thread server_thread([&]() {
    char buf[1024];
    size_t len;
    uint32_t conn_id;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
      server->poll();
      bool did_work = false;
      while (server->recv(buf, sizeof(buf), &len, &conn_id) == 1) {
        server->send(conn_id, buf, len, false);
        did_work = true;
      }
      if (!did_work) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  uint32_t conn_id = client->client_connect("127.0.0.1", 0xC100);
  ASSERT_TRUE(client->is_connected(conn_id));

  const char msg[] = "hello";

  char buf[1024]; size_t len; uint32_t in_conn;
  bool got = false;
  bool sent = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  auto next_send = std::chrono::steady_clock::now();
  while (std::chrono::steady_clock::now() < deadline) {
    auto now = std::chrono::steady_clock::now();
    if (now >= next_send) {
      sent = client->send(conn_id, msg, sizeof(msg), false) == 0 || sent;
      next_send = now + std::chrono::milliseconds(20);
    }
    client->poll();
    int r = client->recv(buf, sizeof(buf), &len, &in_conn);
    if (r == 1) {
      EXPECT_EQ(in_conn, conn_id);
      EXPECT_EQ(len, sizeof(msg));
      EXPECT_STREQ(buf, msg);
      got = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  done.store(true);
  server_thread.join();
  EXPECT_TRUE(sent);
  EXPECT_TRUE(got);

  client->close();
  server->close();
}

TEST(RawUdpSmoke, ReportsCapability) {
  auto a = create_adapter("raw_udp");
  EXPECT_TRUE(a->supports(false));
  EXPECT_FALSE(a->supports(true));
  EXPECT_EQ(a->max_payload_bytes(false), 65507u);
  EXPECT_FALSE(a->encryption_on());
  EXPECT_STREQ(a->name(), "raw_udp");
}

TEST(RawUdpSmoke, EchoManyConnectionsPreservesClientConnIds) {
  constexpr size_t kConns = 128;
  constexpr uint16_t kPort = 0xC101;
  constexpr uint32_t kMarker = 0xAABBCCDDu;

  auto server = create_adapter("raw_udp");
  auto client = create_adapter("raw_udp");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);

  server->server_listen(kPort);

  std::atomic<bool> done{false};
  std::thread server_thread([&]() {
    uint32_t buf[2];
    size_t len;
    uint32_t conn_id;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done.load() && std::chrono::steady_clock::now() < deadline) {
      server->poll();
      bool did_work = false;
      while (server->recv(buf, sizeof(buf), &len, &conn_id) == 1) {
        server->send(conn_id, buf, len, false);
        did_work = true;
      }
      if (!did_work) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  std::vector<uint32_t> conn_ids;
  std::unordered_map<uint32_t, uint32_t> index_by_conn;
  conn_ids.reserve(kConns);
  index_by_conn.reserve(kConns);
  for (uint32_t i = 0; i < kConns; ++i) {
    uint32_t conn_id = client->client_connect("127.0.0.1", kPort);
    conn_ids.push_back(conn_id);
    index_by_conn[conn_id] = i;
  }

  auto send_payload = [&](uint32_t i) {
    uint32_t payload[2] = {kMarker, i};
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(50);
    while (std::chrono::steady_clock::now() < deadline) {
      if (client->send(conn_ids[i], payload, sizeof(payload), false) == 0) return true;
      client->poll();
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    return false;
  };

  for (uint32_t i = 0; i < kConns; ++i) EXPECT_TRUE(send_payload(i));

  std::vector<bool> got(kConns, false);
  size_t received = 0;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  auto next_resend = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
  while (received < kConns && std::chrono::steady_clock::now() < deadline) {
    client->poll();
    for (;;) {
      uint32_t payload[2] = {};
      size_t len = 0;
      uint32_t in_conn = 0;
      int r = client->recv(payload, sizeof(payload), &len, &in_conn);
      if (r == 0) break;
      EXPECT_EQ(r, 1);
      if (r != 1) continue;
      EXPECT_EQ(len, sizeof(payload));
      if (len != sizeof(payload)) continue;
      EXPECT_EQ(payload[0], kMarker);
      if (payload[0] != kMarker) continue;

      auto it = index_by_conn.find(in_conn);
      EXPECT_NE(it, index_by_conn.end());
      if (it == index_by_conn.end()) continue;
      uint32_t expected_index = it->second;
      EXPECT_EQ(payload[1], expected_index);
      if (!got[expected_index]) {
        got[expected_index] = true;
        ++received;
      }
    }
    auto now = std::chrono::steady_clock::now();
    if (received < kConns && now >= next_resend) {
      for (uint32_t i = 0; i < kConns; ++i) {
        if (!got[i]) {
          EXPECT_TRUE(send_payload(i));
        }
      }
      next_resend = now + std::chrono::milliseconds(20);
    }
    if (received < kConns) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  done.store(true);
  server_thread.join();
  EXPECT_EQ(received, kConns);
  client->close();
  server->close();
}
