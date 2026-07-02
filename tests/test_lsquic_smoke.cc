#include <gtest/gtest.h>
#include "harness/adapter_registry.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace rudp_bench { void register_lsquic_adapter(); }

class LsquicRegistrar {
 public:
  LsquicRegistrar() { rudp_bench::register_lsquic_adapter(); }
};
static LsquicRegistrar registrar;

using namespace rudp_bench;

class ScopedEnv {
 public:
  ScopedEnv(const char* name, const char* value) : name_(name) {
    const char* old = ::getenv(name);
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

static void PumpUntilConnected(Adapter& server, Adapter& client, uint32_t cid) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline &&
         !client.is_connected(cid)) {
    client.poll();
    server.poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

TEST(LsquicSmoke, Capability) {
  auto a = create_adapter("lsquic");
  ASSERT_NE(a, nullptr);
  EXPECT_TRUE(a->supports(true));
  EXPECT_TRUE(a->supports(false));
  EXPECT_TRUE(a->encryption_on());
  EXPECT_STREQ(a->name(), "lsquic");
}

TEST(LsquicSmoke, ReliableEcho) {
  auto server = create_adapter("lsquic");
  auto client = create_adapter("lsquic");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);

  constexpr uint16_t kPort = 0xC40C;
  server->server_listen(kPort);
  uint32_t cid = client->client_connect("127.0.0.1", kPort);
  PumpUntilConnected(*server, *client, cid);
  ASSERT_TRUE(client->is_connected(cid));

  const char msg[] = "lsquic-hello";
  ASSERT_EQ(client->send(cid, msg, sizeof(msg), true), 0);

  char server_buf[256];
  char client_buf[256];
  bool server_got = false;
  bool client_got = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline && !client_got) {
    client->poll();
    server->poll();

    if (!server_got) {
      size_t len = 0;
      uint32_t in_cid = 0;
      int rc = server->recv(server_buf, sizeof(server_buf), &len, &in_cid);
      if (rc == 1) {
        ASSERT_EQ(len, sizeof(msg));
        ASSERT_EQ(std::memcmp(server_buf, msg, len), 0);
        ASSERT_EQ(server->send(in_cid, server_buf, len, true), 0);
        server_got = true;
      }
    }

    if (server_got) {
      size_t len = 0;
      uint32_t in_cid = 0;
      int rc = client->recv(client_buf, sizeof(client_buf), &len, &in_cid);
      if (rc == 1) {
        EXPECT_EQ(in_cid, cid);
        ASSERT_EQ(len, sizeof(msg));
        EXPECT_EQ(std::memcmp(client_buf, msg, len), 0);
        client_got = true;
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_TRUE(server_got);
  EXPECT_TRUE(client_got);

  client->close();
  server->close();
}

TEST(LsquicSmoke, PendingWriteCapReturnsBackpressure) {
  ScopedEnv cap("LSQUIC_PENDING_WRITE_BYTES", "8");
  auto server = create_adapter("lsquic");
  auto client = create_adapter("lsquic");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);

  constexpr uint16_t kPort = 0xC40D;
  server->server_listen(kPort);
  uint32_t cid = client->client_connect("127.0.0.1", kPort);
  PumpUntilConnected(*server, *client, cid);
  ASSERT_TRUE(client->is_connected(cid));

  const char msg[] = "too-large";
  EXPECT_EQ(client->send(cid, msg, sizeof(msg), true), -1);
  EXPECT_TRUE(client->is_connected(cid));

  client->close();
  server->close();
}
