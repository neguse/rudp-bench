#include <gtest/gtest.h>
#include "harness/adapter_registry.h"

#include <chrono>
#include <thread>
#include <vector>

namespace rudp_bench { void register_gns_adapter(); }

class GnsRegistrar {
 public:
  GnsRegistrar() { rudp_bench::register_gns_adapter(); }
};
static GnsRegistrar registrar;

using namespace rudp_bench;

// ベンチ既定の "gns" は他アダプタと条件を揃えるため暗号化 OFF。
// 暗号化込みの計測は "gns_encrypted" バリアントで行う。
TEST(GnsSmoke, Capability) {
  auto a = create_adapter("gns");
  ASSERT_NE(a, nullptr);
  EXPECT_TRUE(a->supports(true));
  EXPECT_TRUE(a->supports(false));
  EXPECT_FALSE(a->encryption_on());
  EXPECT_STREQ(a->name(), "gns");

  auto enc = create_adapter("gns_encrypted");
  ASSERT_NE(enc, nullptr);
  EXPECT_TRUE(enc->encryption_on());
}

TEST(GnsSmoke, ReliableEcho) {
  auto server = create_adapter("gns");
  auto client = create_adapter("gns");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);

  // GNS ハンドシェイクは暗号化のため ENet より遅い — タイムアウトを長めに設定
  server->server_listen(0xC107);

  std::thread server_thread([&]() {
    char buf[2048];
    size_t len;
    uint32_t cid;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      server->poll();
      if (server->recv(buf, sizeof(buf), &len, &cid) == 1) {
        server->send(cid, buf, len, true);
        for (int i = 0; i < 20; ++i) {
          server->poll();
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });

  uint32_t cid = client->client_connect("127.0.0.1", 0xC107);

  auto connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < connect_deadline && !client->is_connected(cid)) {
    client->poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(client->is_connected(cid));

  const char msg[] = "gns-hello";
  EXPECT_EQ(client->send(cid, msg, sizeof(msg), true), 0);

  char buf[2048];
  size_t len;
  uint32_t in_cid;
  bool got = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    client->poll();
    if (client->recv(buf, sizeof(buf), &len, &in_cid) == 1) {
      EXPECT_EQ(len, sizeof(msg));
      EXPECT_STREQ(buf, msg);
      got = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(got);

  server_thread.join();
  client->close();
  server->close();
}

TEST(GnsSmoke, DrainsMoreThanOneReceiveBatchPerPoll) {
  constexpr uint16_t kPort = 0xC108;
  constexpr uint32_t kMessages = 96;

  auto server = create_adapter("gns");
  auto client = create_adapter("gns");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);

  server->server_listen(kPort);
  uint32_t cid = client->client_connect("127.0.0.1", kPort);

  auto connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < connect_deadline && !client->is_connected(cid)) {
    server->poll();
    client->poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(client->is_connected(cid));

  for (uint32_t i = 0; i < kMessages; ++i) {
    ASSERT_EQ(client->send(cid, &i, sizeof(i), true), 0);
  }

  for (int i = 0; i < 20; ++i) {
    client->poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  std::vector<bool> got(kMessages, false);
  size_t received = 0;
  size_t max_drained = 0;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (received < kMessages && std::chrono::steady_clock::now() < deadline) {
    server->poll();

    size_t drained = 0;
    for (;;) {
      uint32_t seq = 0;
      size_t len = 0;
      uint32_t in_cid = 0;
      int r = server->recv(&seq, sizeof(seq), &len, &in_cid);
      if (r == 0) break;
      ASSERT_EQ(r, 1);
      ASSERT_EQ(len, sizeof(seq));
      ASSERT_LT(seq, kMessages);
      if (!got[seq]) {
        got[seq] = true;
        ++received;
      }
      ++drained;
    }
    if (drained > max_drained) max_drained = drained;
    if (received < kMessages) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  EXPECT_EQ(received, kMessages);
  EXPECT_GT(max_drained, 64u);

  client->close();
  server->close();
}
