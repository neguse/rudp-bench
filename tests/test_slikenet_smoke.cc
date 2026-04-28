#include <gtest/gtest.h>
#include "harness/adapter_registry.h"

#include <chrono>
#include <thread>

namespace rudp_bench { void register_slikenet_adapter(); }

class SLikeNetRegistrar {
 public:
  SLikeNetRegistrar() { rudp_bench::register_slikenet_adapter(); }
};
static SLikeNetRegistrar registrar;

using namespace rudp_bench;

TEST(SLikeNetSmoke, ReliableEcho) {
  auto server = create_adapter("slikenet");
  auto client = create_adapter("slikenet");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);
  server->server_listen(0xC104);

  std::thread server_thread([&]() {
    char buf[2048]; size_t len; uint32_t cid;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      server->poll();
      if (server->recv(buf, sizeof(buf), &len, &cid) == 1) {
        server->send(cid, buf, len, true);
        // エコー送信後、flush のため数回 poll
        for (int i = 0; i < 20; ++i) {
          server->poll();
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });

  uint32_t cid = client->client_connect("127.0.0.1", 0xC104);

  // is_connected が true になるまで poll
  auto connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < connect_deadline && !client->is_connected(cid)) {
    client->poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(client->is_connected(cid));

  const char msg[] = "slikenet-hello";
  EXPECT_EQ(client->send(cid, msg, sizeof(msg), true), 0);

  char buf[2048]; size_t len; uint32_t in_cid;
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
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_TRUE(got);
  server_thread.join();
  client->close();
  server->close();
}

TEST(SLikeNetSmoke, Capability) {
  auto a = create_adapter("slikenet");
  ASSERT_NE(a, nullptr);
  EXPECT_TRUE(a->supports(true));
  EXPECT_TRUE(a->supports(false));
  EXPECT_FALSE(a->encryption_on());
  EXPECT_STREQ(a->name(), "slikenet");
}
