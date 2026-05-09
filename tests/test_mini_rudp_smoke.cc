#include <gtest/gtest.h>
#include "harness/adapter_registry.h"

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

namespace rudp_bench { void register_mini_rudp_adapter(); }

class MiniRudpRegistrar {
 public:
  MiniRudpRegistrar() { rudp_bench::register_mini_rudp_adapter(); }
};
static MiniRudpRegistrar registrar;

using namespace rudp_bench;

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
  EXPECT_EQ(a->max_payload_bytes(true), 65501u);
  EXPECT_EQ(a->max_payload_bytes(false), 65501u);
  EXPECT_FALSE(a->encryption_on());
  EXPECT_STREQ(a->name(), "mini_rudp");
}
