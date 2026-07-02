#include <gtest/gtest.h>
#include "harness/adapter_registry.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

namespace rudp_bench { void register_slikenet_adapter(); }

class SLikeNetRegistrar {
 public:
  SLikeNetRegistrar() { rudp_bench::register_slikenet_adapter(); }
};
static SLikeNetRegistrar registrar;

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
  std::string old_;
  bool had_old_ = false;
};

}  // namespace

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

TEST(SLikeNetSmoke, OutgoingQueueCapReturnsBackpressure) {
  ScopedEnv cap("SLIKENET_OUTGOING_BYTES", "4096");
  auto server = create_adapter("slikenet");
  auto client = create_adapter("slikenet");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);

  constexpr uint16_t kPort = 0xC105;
  server->server_listen(kPort);
  uint32_t cid = client->client_connect("127.0.0.1", kPort);

  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline &&
         !client->is_connected(cid)) {
    server->poll();
    client->poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(client->is_connected(cid));

  std::string too_large(5000, 'x');
  EXPECT_EQ(client->send(cid, too_large.data(), too_large.size(), true), -1);
  EXPECT_TRUE(client->is_connected(cid));

  const char ok[] = "ok";
  EXPECT_EQ(client->send(cid, ok, sizeof(ok), true), 0);

  client->close();
  server->close();
}

TEST(SLikeNetSmoke, Capability) {
  auto a = create_adapter("slikenet");
  ASSERT_NE(a, nullptr);
  EXPECT_TRUE(a->supports(true));
  EXPECT_TRUE(a->supports(false));
  // 共通 rak_family adapter (per-connection RakPeer) になり多重接続に対応
  EXPECT_EQ(a->max_connections(), 4096u);
  EXPECT_FALSE(a->encryption_on());
  EXPECT_STREQ(a->name(), "slikenet");
}
