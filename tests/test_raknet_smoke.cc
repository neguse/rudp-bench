#include <gtest/gtest.h>

#include "harness/adapter_registry.h"

#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace rudp_bench { void register_raknet_adapter(); }

class RakNetRegistrar {
 public:
  RakNetRegistrar() { rudp_bench::register_raknet_adapter(); }
};
static RakNetRegistrar registrar;

using namespace rudp_bench;

TEST(RakNetSmoke, MultiConnectionReliableEcho) {
  auto server = create_adapter("raknet");
  auto client = create_adapter("raknet");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);

  constexpr uint16_t kPort = 0xC501;
  constexpr uint32_t kConnCount = 3;
  server->hint_connections(kConnCount);
  client->hint_connections(kConnCount);
  server->server_listen(kPort);

  std::vector<uint32_t> ids;
  ids.reserve(kConnCount);
  for (uint32_t i = 0; i < kConnCount; ++i) {
    ids.push_back(client->client_connect("127.0.0.1", kPort));
  }

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    server->poll();
    client->poll();
    bool all_connected = true;
    for (uint32_t id : ids) {
      all_connected = all_connected && client->is_connected(id);
    }
    if (all_connected) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  for (uint32_t id : ids) {
    ASSERT_TRUE(client->is_connected(id));
  }

  std::unordered_map<std::string, uint32_t> expected;
  for (uint32_t i = 0; i < kConnCount; ++i) {
    std::string msg = "raknet-hello-" + std::to_string(i);
    expected[msg] = ids[i];
    ASSERT_EQ(client->send(ids[i], msg.data(), msg.size() + 1, true), 0);
  }

  std::unordered_set<std::string> got;
  std::vector<char> buf(2048);
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline &&
         got.size() < expected.size()) {
    server->poll();
    for (;;) {
      size_t len = 0;
      uint32_t cid = 0;
      int r = server->recv(buf.data(), buf.size(), &len, &cid);
      if (r != 1) break;
      ASSERT_EQ(server->send(cid, buf.data(), len, true), 0);
    }

    client->poll();
    for (;;) {
      size_t len = 0;
      uint32_t cid = 0;
      int r = client->recv(buf.data(), buf.size(), &len, &cid);
      if (r != 1) break;
      ASSERT_GT(len, 0u);
      std::string msg(buf.data());
      auto it = expected.find(msg);
      ASSERT_NE(it, expected.end());
      EXPECT_EQ(cid, it->second);
      got.insert(msg);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  EXPECT_EQ(got.size(), expected.size());
  client->close();
  server->close();
}

TEST(RakNetSmoke, Capability) {
  auto a = create_adapter("raknet");
  ASSERT_NE(a, nullptr);
  EXPECT_TRUE(a->supports(true));
  EXPECT_TRUE(a->supports(false));
  EXPECT_EQ(a->max_connections(), 4096u);
  EXPECT_EQ(a->max_payload_bytes(true), 65536u);
  EXPECT_EQ(a->max_payload_bytes(false), 65536u);
  EXPECT_FALSE(a->encryption_on());
  EXPECT_STREQ(a->name(), "raknet");
}
