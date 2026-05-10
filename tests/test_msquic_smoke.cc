#include <gtest/gtest.h>
#include "harness/adapter_registry.h"

#include <chrono>
#include <cstring>
#include <thread>

namespace rudp_bench { void register_msquic_adapter(); }

class MsquicRegistrar {
 public:
  MsquicRegistrar() { rudp_bench::register_msquic_adapter(); }
};
static MsquicRegistrar registrar;

using namespace rudp_bench;

TEST(MsquicSmoke, Capability) {
  auto a = create_adapter("msquic");
  ASSERT_NE(a, nullptr);
  EXPECT_TRUE(a->supports(true));
  EXPECT_TRUE(a->supports(false));
  EXPECT_TRUE(a->encryption_on());
  EXPECT_STREQ(a->name(), "msquic");
}

TEST(MsquicSmoke, ReliableEcho) {
  auto server = create_adapter("msquic");
  auto client = create_adapter("msquic");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);

  server->server_listen(0xC208);

  std::thread server_thread([&]() {
    char buf[2048];
    size_t len;
    uint32_t cid;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
      server->poll();
      if (server->recv(buf, sizeof(buf), &len, &cid) == 1) {
        server->send(cid, buf, len, true);
        for (int i = 0; i < 50; ++i) {
          server->poll();
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });

  uint32_t cid = client->client_connect("127.0.0.1", 0xC208);

  auto connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < connect_deadline && !client->is_connected(cid)) {
    client->poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(client->is_connected(cid));

  const char msg[] = "msquic-hello";
  EXPECT_EQ(client->send(cid, msg, sizeof(msg), true), 0);

  char buf[2048];
  size_t len;
  uint32_t in_cid;
  bool got = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
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


TEST(MsquicSmoke, MultipleReliableMessagesUseOneConnection) {
  auto server = create_adapter("msquic");
  auto client = create_adapter("msquic");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);

  server->server_listen(0xC20A);

  std::thread server_thread([&]() {
    char buf[2048];
    size_t len;
    uint32_t cid;
    int echoed = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline && echoed < 2) {
      server->poll();
      if (server->recv(buf, sizeof(buf), &len, &cid) == 1) {
        server->send(cid, buf, len, true);
        ++echoed;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    for (int i = 0; i < 50; ++i) {
      server->poll();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });

  uint32_t cid = client->client_connect("127.0.0.1", 0xC20A);

  auto connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < connect_deadline && !client->is_connected(cid)) {
    client->poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(client->is_connected(cid));

  const char msg0[] = "msquic-reliable-0";
  const char msg1[] = "msquic-reliable-1";
  EXPECT_EQ(client->send(cid, msg0, sizeof(msg0), true), 0);
  EXPECT_EQ(client->send(cid, msg1, sizeof(msg1), true), 0);

  char buf[2048];
  size_t len;
  uint32_t in_cid;
  bool got0 = false;
  bool got1 = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline && (!got0 || !got1)) {
    client->poll();
    if (client->recv(buf, sizeof(buf), &len, &in_cid) == 1) {
      if (len == sizeof(msg0) && std::memcmp(buf, msg0, sizeof(msg0)) == 0) got0 = true;
      if (len == sizeof(msg1) && std::memcmp(buf, msg1, sizeof(msg1)) == 0) got1 = true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(got0);
  EXPECT_TRUE(got1);

  server_thread.join();
  client->close();
  server->close();
}

TEST(MsquicSmoke, UnreliableEcho) {
  auto server = create_adapter("msquic");
  auto client = create_adapter("msquic");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);

  server->server_listen(0xC209);

  std::thread server_thread([&]() {
    char buf[2048];
    size_t len;
    uint32_t cid;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
      server->poll();
      if (server->recv(buf, sizeof(buf), &len, &cid) == 1) {
        server->send(cid, buf, len, false);
        for (int i = 0; i < 50; ++i) {
          server->poll();
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  });

  uint32_t cid = client->client_connect("127.0.0.1", 0xC209);

  auto connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < connect_deadline && !client->is_connected(cid)) {
    client->poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(client->is_connected(cid));

  const char msg[] = "msquic-dgram";
  EXPECT_EQ(client->send(cid, msg, sizeof(msg), false), 0);

  char buf[2048];
  size_t len;
  uint32_t in_cid;
  bool got = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
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
