#include <gtest/gtest.h>
#include "harness/adapter_registry.h"

#include <thread>
#include <chrono>
#include <cstring>

namespace rudp_bench { void register_raw_udp_adapter(); }

class RawUdpRegistrar {
 public:
  RawUdpRegistrar() { rudp_bench::register_raw_udp_adapter(); }
};
static RawUdpRegistrar registrar;

using namespace rudp_bench;

TEST(RawUdpSmoke, EchoOneMessage) {
  auto server = create_adapter("raw_udp");
  auto client = create_adapter("raw_udp");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);

  server->server_listen(0xC100);

  std::thread server_thread([&]() {
    char buf[1024];
    size_t len;
    uint32_t conn_id;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      server->poll();
      int r = server->recv(buf, sizeof(buf), &len, &conn_id);
      if (r == 1) {
        server->send(conn_id, buf, len, false);
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  uint32_t conn_id = client->client_connect("127.0.0.1", 0xC100);
  ASSERT_TRUE(client->is_connected(conn_id));

  const char msg[] = "hello";
  ASSERT_EQ(client->send(conn_id, msg, sizeof(msg), false), 0);

  char buf[1024]; size_t len; uint32_t in_conn;
  bool got = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    client->poll();
    int r = client->recv(buf, sizeof(buf), &len, &in_conn);
    if (r == 1) {
      EXPECT_EQ(in_conn, conn_id);
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

TEST(RawUdpSmoke, ReportsCapability) {
  auto a = create_adapter("raw_udp");
  EXPECT_TRUE(a->supports(false));
  EXPECT_FALSE(a->supports(true));
  EXPECT_EQ(a->max_payload_bytes(false), 65507u);
  EXPECT_FALSE(a->encryption_on());
  EXPECT_STREQ(a->name(), "raw_udp");
}
