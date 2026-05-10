#include <gtest/gtest.h>
#include "harness/adapter_registry.h"

#include <chrono>
#include <thread>

namespace rudp_bench { void register_enet_adapter(); }

class EnetRegistrar {
 public:
  EnetRegistrar() { rudp_bench::register_enet_adapter(); }
};
static EnetRegistrar registrar;

using namespace rudp_bench;

TEST(EnetSmoke, ReliableEcho) {
  auto server = create_adapter("enet");
  auto client = create_adapter("enet");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);
  server->server_listen(0xC102);

  std::thread server_thread([&]() {
    char buf[2048]; size_t len; uint32_t cid;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
      server->poll();
      if (server->recv(buf, sizeof(buf), &len, &cid) == 1) {
        server->send(cid, buf, len, true);
        // poll once more to flush
        for (int i = 0; i < 10; ++i) {
          server->poll();
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });

  uint32_t cid = client->client_connect("127.0.0.1", 0xC102);

  // is_connected が true になるまで poll
  auto connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < connect_deadline && !client->is_connected(cid)) {
    client->poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(client->is_connected(cid));

  const char msg[] = "enet-hello";
  EXPECT_EQ(client->send(cid, msg, sizeof(msg), true), 0);

  char buf[2048]; size_t len; uint32_t in_cid;
  bool got = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    client->poll();
    if (client->recv(buf, sizeof(buf), &len, &in_cid) == 1) {
      EXPECT_EQ(len, sizeof(msg));
      EXPECT_STREQ(buf, msg);
      got = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_TRUE(got);
  server_thread.join();
  client->close();
  server->close();
}

TEST(EnetSmoke, Capability) {
  auto a = create_adapter("enet");
  ASSERT_NE(a, nullptr);
  EXPECT_TRUE(a->supports(true));
  EXPECT_TRUE(a->supports(false));
  EXPECT_EQ(a->max_connections(), 4095u);
  EXPECT_FALSE(a->encryption_on());
  EXPECT_STREQ(a->name(), "enet");
}
