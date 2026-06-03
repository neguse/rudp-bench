#include <gtest/gtest.h>

#include "harness/adapter_registry.h"

#include <chrono>
#include <cstring>
#include <thread>

namespace rudp_bench { void register_coop_rudp_adapter(); }

namespace {

class CoopRudpRegistrar {
 public:
  CoopRudpRegistrar() { rudp_bench::register_coop_rudp_adapter(); }
};
static CoopRudpRegistrar registrar;

void run_echo(bool reliable, uint16_t port, const char* msg) {
  auto server = rudp_bench::create_adapter("coop_rudp");
  auto client = rudp_bench::create_adapter("coop_rudp");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);
  server->server_listen(port);

  std::thread server_thread([&]() {
    char buf[2048];
    size_t len = 0;
    uint32_t cid = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      server->poll();
      if (server->recv(buf, sizeof(buf), &len, &cid) == 1) {
        server->send(cid, buf, len, reliable);
        server->poll();
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  uint32_t cid = client->client_connect("127.0.0.1", port);
  ASSERT_NE(cid, 0u);
  ASSERT_EQ(client->send(cid, msg, std::strlen(msg) + 1, reliable), 0);

  char buf[2048];
  size_t len = 0;
  uint32_t in_cid = 0;
  bool got = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    client->poll();
    if (client->recv(buf, sizeof(buf), &len, &in_cid) == 1) {
      EXPECT_EQ(in_cid, cid);
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

}  // namespace

TEST(CoopRudpAdapterSmoke, ReliableEcho) {
  run_echo(true, 0xC401, "coop-reliable");
}

TEST(CoopRudpAdapterSmoke, UnreliableEcho) {
  run_echo(false, 0xC402, "coop-unreliable");
}
