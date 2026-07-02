#include <gtest/gtest.h>
#include "harness/adapter_registry.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace rudp_bench { void register_quiche_adapter(); }

class QuicheRegistrar {
 public:
  QuicheRegistrar() { rudp_bench::register_quiche_adapter(); }
};
static QuicheRegistrar registrar;

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

TEST(QuicheSmoke, Capability) {
  auto a = create_adapter("quiche");
  ASSERT_NE(a, nullptr);
  EXPECT_TRUE(a->supports(true));
  EXPECT_TRUE(a->supports(false));
  EXPECT_TRUE(a->encryption_on());
  EXPECT_STREQ(a->name(), "quiche");
}

TEST(QuicheSmoke, LargeReliableEchoFlushesPartialStreamWrite) {
  auto server = create_adapter("quiche");
  auto client = create_adapter("quiche");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);

  constexpr uint16_t kPort = 0xC30C;
  server->server_listen(kPort);
  uint32_t cid = client->client_connect("127.0.0.1", kPort);
  PumpUntilConnected(*server, *client, cid);
  ASSERT_TRUE(client->is_connected(cid));

  std::vector<uint8_t> payload(client->max_payload_bytes(true));
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<uint8_t>(i * 31u);
  }
  ASSERT_EQ(client->send(cid, payload.data(), payload.size(), true), 0);

  std::vector<uint8_t> server_buf(payload.size());
  std::vector<uint8_t> client_buf(payload.size());
  bool server_got = false;
  bool client_got = false;
  uint32_t server_cid = 0;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline && !client_got) {
    client->poll();
    server->poll();

    if (!server_got) {
      size_t len = 0;
      uint32_t in_cid = 0;
      int rc = server->recv(server_buf.data(), server_buf.size(), &len,
                            &in_cid);
      if (rc == 1) {
        ASSERT_EQ(len, payload.size());
        ASSERT_EQ(std::memcmp(server_buf.data(), payload.data(), len), 0);
        ASSERT_EQ(server->send(in_cid, server_buf.data(), len, true), 0);
        server_cid = in_cid;
        server_got = true;
      }
    }

    if (server_got) {
      size_t len = 0;
      uint32_t in_cid = 0;
      int rc = client->recv(client_buf.data(), client_buf.size(), &len,
                            &in_cid);
      if (rc == 1) {
        EXPECT_EQ(in_cid, cid);
        ASSERT_EQ(len, payload.size());
        EXPECT_EQ(std::memcmp(client_buf.data(), payload.data(), len), 0);
        client_got = true;
      }
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  EXPECT_TRUE(server_got);
  EXPECT_NE(server_cid, 0u);
  EXPECT_TRUE(client_got);

  client->close();
  server->close();
}

TEST(QuicheSmoke, StreamPendingCapReturnsBackpressure) {
  ScopedEnv cap("QUICHE_STREAM_PENDING_BYTES", "8");
  auto server = create_adapter("quiche");
  auto client = create_adapter("quiche");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);

  constexpr uint16_t kPort = 0xC30D;
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
