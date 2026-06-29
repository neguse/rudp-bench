#include <gtest/gtest.h>
#include "harness/adapter_registry.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace rudp_bench { void register_msquic_adapter(); }

class MsquicRegistrar {
 public:
  MsquicRegistrar() { rudp_bench::register_msquic_adapter(); }
};
static MsquicRegistrar registrar;

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

TEST(MsquicSmoke, InboxLimitDropsOldestMessages) {
  ScopedEnv limit("MSQUIC_INBOX_MESSAGES", "2");
  constexpr uint16_t kPort = 0xC20B;
  constexpr uint32_t kMessages = 4;

  auto server = create_adapter("msquic");
  auto client = create_adapter("msquic");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);

  server->server_listen(kPort);
  uint32_t cid = client->client_connect("127.0.0.1", kPort);

  auto connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < connect_deadline && !client->is_connected(cid)) {
    client->poll();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(client->is_connected(cid));

  for (uint32_t i = 0; i < kMessages; ++i) {
    ASSERT_EQ(client->send(cid, &i, sizeof(i), true), 0);
  }

  std::this_thread::sleep_for(std::chrono::seconds(1));

  std::vector<uint32_t> got;
  for (;;) {
    uint32_t seq = 0;
    size_t len = 0;
    uint32_t in_cid = 0;
    int r = server->recv(&seq, sizeof(seq), &len, &in_cid);
    if (r == 0) break;
    ASSERT_EQ(r, 1);
    ASSERT_EQ(len, sizeof(seq));
    got.push_back(seq);
  }

  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[0], 2u);
  EXPECT_EQ(got[1], 3u);

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
