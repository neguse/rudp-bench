#include <gtest/gtest.h>
#include "harness/adapter_registry.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

namespace rudp_bench { void register_kcp_adapter(); }

class KcpRegistrar {
 public:
  KcpRegistrar() { rudp_bench::register_kcp_adapter(); }
};
static KcpRegistrar registrar;

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

TEST(KcpSmoke, DeadLinkStateMarksConnectionDisconnected) {
  ScopedEnv dead_link("KCP_DEAD_LINK", "2");
  auto client = create_adapter("kcp");
  ASSERT_NE(client, nullptr);

  uint32_t cid = client->client_connect("127.0.0.1", 0xC1FD);
  ASSERT_TRUE(client->is_connected(cid));

  const char msg[] = "dead-link";
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

TEST(KcpSmoke, ReliableSendQueueCapReturnsBackpressure) {
  ScopedEnv cap("KCP_SEND_QUEUE_BYTES", "8");
  auto client = create_adapter("kcp");
  ASSERT_NE(client, nullptr);

  uint32_t cid = client->client_connect("127.0.0.1", 0xC1FE);
  ASSERT_TRUE(client->is_connected(cid));

  const char too_large[] = "too-large";
  EXPECT_EQ(client->send(cid, too_large, sizeof(too_large), true), -1);
  EXPECT_TRUE(client->is_connected(cid));

  const char ok[] = "ok";
  EXPECT_EQ(client->send(cid, ok, sizeof(ok), true), 0);
  client->close();
}

TEST(KcpSmoke, ReliableEcho) {
  auto server = create_adapter("kcp");
  auto client = create_adapter("kcp");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);
  server->server_listen(0xC103);

  std::thread server_thread([&]() {
    char buf[2048]; size_t len; uint32_t cid;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
      server->poll();
      if (server->recv(buf, sizeof(buf), &len, &cid) == 1) {
        server->send(cid, buf, len, true);
        // poll しばらく回して flush
        for (int i = 0; i < 20; ++i) {
          server->poll();
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });

  uint32_t cid = client->client_connect("127.0.0.1", 0xC103);

  // is_connected が true になるまで poll (KCP は即時 ready)
  auto connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < connect_deadline && !client->is_connected(cid)) {
    client->poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(client->is_connected(cid));

  const char msg[] = "kcp-hello";
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

TEST(KcpSmoke, UnreliableEcho) {
  auto server = create_adapter("kcp");
  auto client = create_adapter("kcp");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);
  // 別ポートで unreliable テスト
  server->server_listen(0xC104);

  std::thread server_thread([&]() {
    char buf[2048]; size_t len; uint32_t cid;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
      server->poll();
      if (server->recv(buf, sizeof(buf), &len, &cid) == 1) {
        server->send(cid, buf, len, false);
        for (int i = 0; i < 10; ++i) {
          server->poll();
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });

  uint32_t cid = client->client_connect("127.0.0.1", 0xC104);
  ASSERT_TRUE(client->is_connected(cid));

  const char msg[] = "kcp-unreliable";
  EXPECT_EQ(client->send(cid, msg, sizeof(msg), false), 0);

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

TEST(KcpSmoke, Capability) {
  auto a = create_adapter("kcp");
  ASSERT_NE(a, nullptr);
  EXPECT_TRUE(a->supports(true));
  EXPECT_TRUE(a->supports(false));
  EXPECT_FALSE(a->encryption_on());
  EXPECT_STREQ(a->name(), "kcp");
}
