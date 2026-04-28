#include <gtest/gtest.h>
#include "harness/adapter_registry.h"

#include <chrono>
#include <thread>

namespace rudp_bench { void register_gns_adapter(); }

class GnsRegistrar {
 public:
  GnsRegistrar() { rudp_bench::register_gns_adapter(); }
};
static GnsRegistrar registrar;

using namespace rudp_bench;

// GNS はデフォルトで暗号化 ON であることを検証する
TEST(GnsSmoke, Capability) {
  auto a = create_adapter("gns");
  ASSERT_NE(a, nullptr);
  EXPECT_TRUE(a->supports(true));
  EXPECT_TRUE(a->supports(false));
  EXPECT_TRUE(a->encryption_on());
  EXPECT_STREQ(a->name(), "gns");
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
