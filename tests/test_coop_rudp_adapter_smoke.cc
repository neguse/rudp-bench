#include <gtest/gtest.h>

#include "harness/adapter_registry.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace rudp_bench { void register_coop_rudp_adapter(); }

namespace {

class CoopRudpRegistrar {
 public:
  CoopRudpRegistrar() { rudp_bench::register_coop_rudp_adapter(); }
};
static CoopRudpRegistrar registrar;

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

constexpr uint32_t kPhysicalBatchMagic = 0x31425243u;
constexpr uint32_t kRudpPacketMagic = 0x52554400u;
constexpr size_t kRudpPacketHeaderBytes = 32;
constexpr size_t kRudpDataFrameHeaderBytes = 20;

void store_u16_le(std::vector<uint8_t>& bytes, size_t off, uint16_t v) {
  bytes[off + 0] = static_cast<uint8_t>(v & 0xffu);
  bytes[off + 1] = static_cast<uint8_t>((v >> 8) & 0xffu);
}

void store_u32_le(std::vector<uint8_t>& bytes, size_t off, uint32_t v) {
  bytes[off + 0] = static_cast<uint8_t>(v & 0xffu);
  bytes[off + 1] = static_cast<uint8_t>((v >> 8) & 0xffu);
  bytes[off + 2] = static_cast<uint8_t>((v >> 16) & 0xffu);
  bytes[off + 3] = static_cast<uint8_t>((v >> 24) & 0xffu);
}

void store_u64_le(std::vector<uint8_t>& bytes, size_t off, uint64_t v) {
  for (size_t i = 0; i < 8; ++i) {
    bytes[off + i] = static_cast<uint8_t>((v >> (i * 8)) & 0xffu);
  }
}

std::vector<uint8_t> make_raw_rudp_packet(uint64_t conn_id, uint32_t packet_seq,
                                          const char* msg) {
  size_t msg_len = std::strlen(msg) + 1;
  std::vector<uint8_t> bytes(kRudpPacketHeaderBytes + kRudpDataFrameHeaderBytes +
                             msg_len);
  store_u32_le(bytes, 0, kRudpPacketMagic);
  store_u64_le(bytes, 4, conn_id);
  store_u32_le(bytes, 12, packet_seq);
  store_u32_le(bytes, 16, 0);
  store_u64_le(bytes, 20, 0);
  store_u16_le(bytes, 28, static_cast<uint16_t>(kRudpPacketHeaderBytes));
  store_u16_le(bytes, 30, static_cast<uint16_t>(kRudpDataFrameHeaderBytes + msg_len));

  size_t off = kRudpPacketHeaderBytes;
  bytes[off + 0] = 1;
  bytes[off + 1] = 0;
  store_u16_le(bytes, off + 2, static_cast<uint16_t>(kRudpDataFrameHeaderBytes));
  store_u16_le(bytes, off + 4, 0);
  store_u16_le(bytes, off + 6, static_cast<uint16_t>(msg_len));
  store_u32_le(bytes, off + 8, 1);
  store_u16_le(bytes, off + 12, 0);
  store_u16_le(bytes, off + 14, 1);
  store_u16_le(bytes, off + 16, 0);
  store_u16_le(bytes, off + 18, 0);
  std::memcpy(bytes.data() + off + kRudpDataFrameHeaderBytes, msg, msg_len);
  return bytes;
}

void send_raw_udp(int fd, uint16_t port, const std::vector<uint8_t>& bytes) {
  sockaddr_in dst{};
  dst.sin_family = AF_INET;
  dst.sin_port = htons(port);
  ASSERT_EQ(inet_pton(AF_INET, "127.0.0.1", &dst.sin_addr), 1);
  ASSERT_EQ(::sendto(fd, bytes.data(), bytes.size(), 0,
                     reinterpret_cast<sockaddr*>(&dst), sizeof(dst)),
            static_cast<ssize_t>(bytes.size()));
}

void append_physical_frame(std::vector<uint8_t>& batch, const uint8_t* data,
                           size_t len) {
  size_t off = batch.size();
  batch.resize(off + 2 + len);
  store_u16_le(batch, off, static_cast<uint16_t>(len));
  if (len != 0) std::memcpy(batch.data() + off + 2, data, len);
}

void run_echo_once(rudp_bench::Adapter& server, rudp_bench::Adapter& client,
                   bool reliable, uint16_t port, const char* msg) {
  server.server_listen(port);

  uint32_t cid = client.client_connect("127.0.0.1", port);
  ASSERT_NE(cid, 0u);
  ASSERT_EQ(client.send(cid, msg, std::strlen(msg) + 1, reliable), 0);

  char server_buf[2048];
  char buf[2048];
  size_t server_len = 0;
  size_t len = 0;
  uint32_t server_cid = 0;
  uint32_t in_cid = 0;
  bool server_got = false;
  bool got = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    client.poll();
    server.poll();
    if (!server_got &&
        server.recv(server_buf, sizeof(server_buf), &server_len, &server_cid) == 1) {
      EXPECT_EQ(server.send(server_cid, server_buf, server_len, reliable), 0);
      server.poll();
      server_got = true;
    }
    if (client.recv(buf, sizeof(buf), &len, &in_cid) == 1) {
      EXPECT_EQ(in_cid, cid);
      EXPECT_STREQ(buf, msg);
      got = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  EXPECT_TRUE(server_got);
  EXPECT_TRUE(got);
}

void run_echo(bool reliable, uint16_t port, const char* msg) {
  auto server = rudp_bench::create_adapter("coop_rudp");
  auto client = rudp_bench::create_adapter("coop_rudp");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);
  run_echo_once(*server, *client, reliable, port, msg);
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

TEST(CoopRudpAdapterSmoke, UnreliableLargePayloadEchoesFullBytes) {
  auto server = rudp_bench::create_adapter("coop_rudp");
  auto client = rudp_bench::create_adapter("coop_rudp");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);
  server->server_listen(0xC403);

  std::vector<uint8_t> payload(client->max_payload_bytes(false));
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<uint8_t>((i * 13u) & 0xffu);
  }

  std::thread server_thread([&]() {
    uint8_t buf[2048]{};
    size_t len = 0;
    uint32_t cid = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
      server->poll();
      if (server->recv(buf, sizeof(buf), &len, &cid) == 1) {
        server->send(cid, buf, len, false);
        server->poll();
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  uint32_t cid = client->client_connect("127.0.0.1", 0xC403);
  ASSERT_NE(cid, 0u);
  ASSERT_EQ(client->send(cid, payload.data(), payload.size(), false), 0);

  std::vector<uint8_t> buf(2048);
  size_t len = 0;
  uint32_t in_cid = 0;
  bool got = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    client->poll();
    if (client->recv(buf.data(), buf.size(), &len, &in_cid) == 1) {
      EXPECT_EQ(in_cid, cid);
      EXPECT_EQ(len, payload.size());
      EXPECT_EQ(std::memcmp(buf.data(), payload.data(), payload.size()), 0);
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

TEST(CoopRudpAdapterSmoke, MalformedPhysicalBatchDoesNotDeliverPrefix) {
  auto server = rudp_bench::create_adapter("coop_rudp");
  ASSERT_NE(server, nullptr);
  constexpr uint16_t kPort = 0xC40F;
  server->server_listen(kPort);

  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(fd, 0);

  constexpr uint32_t kConnId = 0x00ABCDEFu;
  std::vector<uint8_t> inner =
      make_raw_rudp_packet(kConnId, /*packet_seq=*/1, "should-not-arrive");
  std::vector<uint8_t> malformed(4 + 2 + inner.size() + 1);
  store_u32_le(malformed, 0, kPhysicalBatchMagic);
  store_u16_le(malformed, 4, static_cast<uint16_t>(inner.size()));
  std::memcpy(malformed.data() + 6, inner.data(), inner.size());
  malformed.back() = 0xee;
  send_raw_udp(fd, kPort, malformed);

  char buf[64]{};
  size_t len = 0;
  uint32_t in_cid = 0;
  bool got_bad = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  while (std::chrono::steady_clock::now() < deadline) {
    server->poll();
    if (server->recv(buf, sizeof(buf), &len, &in_cid) == 1) {
      got_bad = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_FALSE(got_bad);

  const char good[] = "valid-after-bad";
  std::vector<uint8_t> valid = make_raw_rudp_packet(kConnId, /*packet_seq=*/2, good);
  send_raw_udp(fd, kPort, valid);

  bool got_good = false;
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    server->poll();
    if (server->recv(buf, sizeof(buf), &len, &in_cid) == 1) {
      got_good = in_cid == kConnId && std::memcmp(buf, good, sizeof(good)) == 0;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  EXPECT_TRUE(got_good);
  ::close(fd);
  server->close();
}

TEST(CoopRudpAdapterSmoke, IncomingConnIdAboveAdapterRangeIsNotExposed) {
  auto server = rudp_bench::create_adapter("coop_rudp");
  ASSERT_NE(server, nullptr);
  constexpr uint16_t kPort = 0xC412;
  server->server_listen(kPort);

  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(fd, 0);

  for (uint64_t i = 0; i < 80; ++i) {
    std::vector<uint8_t> high =
        make_raw_rudp_packet(0x1'0000'0001ull + i, /*packet_seq=*/1,
                             "should-not-expose");
    send_raw_udp(fd, kPort, high);
  }

  char buf[64]{};
  size_t len = 0;
  uint32_t in_cid = 0;
  bool got_bad = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
  while (std::chrono::steady_clock::now() < deadline) {
    server->poll();
    if (server->recv(buf, sizeof(buf), &len, &in_cid) == 1) {
      got_bad = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_FALSE(got_bad);

  constexpr uint32_t kGoodConnId = 0x00ABCDF0u;
  const char good[] = "valid-32-bit";
  std::vector<uint8_t> valid =
      make_raw_rudp_packet(kGoodConnId, /*packet_seq=*/1, good);
  send_raw_udp(fd, kPort, valid);

  bool got_good = false;
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    server->poll();
    if (server->recv(buf, sizeof(buf), &len, &in_cid) == 1) {
      got_good = in_cid == kGoodConnId &&
                 std::memcmp(buf, good, sizeof(good)) == 0;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  EXPECT_TRUE(got_good);
  ::close(fd);
  server->close();
}

TEST(CoopRudpAdapterSmoke, PhysicalBatchDropsInvalidFramesBeforePending) {
  auto server = rudp_bench::create_adapter("coop_rudp");
  ASSERT_NE(server, nullptr);
  constexpr uint16_t kPort = 0xC413;
  server->server_listen(kPort);

  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  ASSERT_GE(fd, 0);

  constexpr uint32_t kConnId = 0x00ABCDF1u;
  const char good[] = "valid-after-invalid-batch-frames";
  std::vector<uint8_t> valid =
      make_raw_rudp_packet(kConnId, /*packet_seq=*/1, good);

  std::vector<uint8_t> batch(4);
  store_u32_le(batch, 0, kPhysicalBatchMagic);
  const uint8_t invalid_frame[] = {0xee};
  for (size_t i = 0; i < 9000; ++i) {
    append_physical_frame(batch, invalid_frame, sizeof(invalid_frame));
  }
  append_physical_frame(batch, valid.data(), valid.size());
  ASSERT_LT(batch.size(), 65507u);
  send_raw_udp(fd, kPort, batch);

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  server->poll();

  char buf[64]{};
  size_t len = 0;
  uint32_t in_cid = 0;
  ASSERT_EQ(server->recv(buf, sizeof(buf), &len, &in_cid), 1);
  EXPECT_EQ(in_cid, kConnId);
  EXPECT_EQ(std::memcmp(buf, good, sizeof(good)), 0);

  ::close(fd);
  server->close();
}

TEST(CoopRudpAdapterSmoke, RecvTooSmallPreservesPayload) {
  auto server = rudp_bench::create_adapter("coop_rudp");
  auto client = rudp_bench::create_adapter("coop_rudp");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);
  server->server_listen(0xC404);

  std::vector<uint8_t> payload(64);
  for (size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<uint8_t>(i ^ 0x5a);
  }

  uint32_t cid = client->client_connect("127.0.0.1", 0xC404);
  ASSERT_NE(cid, 0u);
  ASSERT_EQ(client->send(cid, payload.data(), payload.size(), false), 0);

  uint8_t small[4]{};
  std::vector<uint8_t> buf(128);
  size_t len = 0;
  uint32_t in_cid = 0;
  bool got = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    client->poll();
    server->poll();
    int r = server->recv(small, sizeof(small), &len, &in_cid);
    if (r == -1) {
      EXPECT_EQ(len, payload.size());
      EXPECT_EQ(in_cid, cid);
      ASSERT_EQ(server->recv(buf.data(), buf.size(), &len, &in_cid), 1);
      EXPECT_EQ(len, payload.size());
      EXPECT_EQ(std::memcmp(buf.data(), payload.data(), payload.size()), 0);
      got = true;
      break;
    }
    EXPECT_EQ(r, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  EXPECT_TRUE(got);
  client->close();
  server->close();
}

TEST(CoopRudpAdapterSmoke, ReportsBoundedMaxConnections) {
  auto adapter = rudp_bench::create_adapter("coop_rudp");
  ASSERT_NE(adapter, nullptr);
  EXPECT_EQ(adapter->max_connections(), 4096u);
  EXPECT_EQ(adapter->max_payload_bytes(false), 1148u);
  EXPECT_EQ(adapter->max_payload_bytes(true), 1148u);
}

TEST(CoopRudpAdapterSmoke, ClientConnectRejectsNullHost) {
  auto server = rudp_bench::create_adapter("coop_rudp");
  auto client = rudp_bench::create_adapter("coop_rudp");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);
  server->server_listen(0xC40B);

  EXPECT_EQ(client->client_connect(nullptr, 0xC40B), 0u);
  EXPECT_EQ(client->client_connect("not-an-ip-address", 0xC40B), 0u);
  uint32_t cid = client->client_connect("127.0.0.1", 0xC40B);
  EXPECT_NE(cid, 0u);

  client->close();
  server->close();
}

TEST(CoopRudpAdapterSmoke, SendManyRejectsNullConnList) {
  auto server = rudp_bench::create_adapter("coop_rudp");
  auto client = rudp_bench::create_adapter("coop_rudp");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);
  server->server_listen(0xC40C);

  const char msg[] = "send-many-null";
  uint32_t cid = client->client_connect("127.0.0.1", 0xC40C);
  ASSERT_NE(cid, 0u);
  EXPECT_EQ(client->send_many(nullptr, 1, msg, sizeof(msg), false), 0u);
  EXPECT_EQ(client->send_many(nullptr, 0, msg, sizeof(msg), false), 0u);

  uint32_t ids[] = {cid};
  EXPECT_EQ(client->send_many(ids, 1, msg, sizeof(msg), false), 1u);

  client->close();
  server->close();
}

TEST(CoopRudpAdapterSmoke, ServerConnectionPeakTracksObservedConnections) {
  auto server = rudp_bench::create_adapter("coop_rudp");
  auto client = rudp_bench::create_adapter("coop_rudp");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);
  server->hint_connections(2);
  server->server_listen(0xC407);
  EXPECT_EQ(server->connection_stats().connected_peak, 0u);

  const char msg[] = "peak";
  uint32_t cid = client->client_connect("127.0.0.1", 0xC407);
  ASSERT_NE(cid, 0u);
  ASSERT_EQ(client->send(cid, msg, sizeof(msg), false), 0);

  char buf[64]{};
  size_t len = 0;
  uint32_t in_cid = 0;
  bool got = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    client->poll();
    server->poll();
    if (server->recv(buf, sizeof(buf), &len, &in_cid) == 1) {
      got = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  EXPECT_TRUE(got);
  EXPECT_EQ(server->connection_stats().connected_peak, 1u);
  client->close();
  server->close();
}

TEST(CoopRudpAdapterSmoke, ServerConnectionPeakCanExceedHint) {
  auto server = rudp_bench::create_adapter("coop_rudp");
  auto client = rudp_bench::create_adapter("coop_rudp");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);
  server->hint_connections(1);
  server->server_listen(0xC40D);

  const char msg[] = "peak-over-hint";
  uint32_t c0 = client->client_connect("127.0.0.1", 0xC40D);
  uint32_t c1 = client->client_connect("127.0.0.1", 0xC40D);
  ASSERT_NE(c0, 0u);
  ASSERT_NE(c1, 0u);
  ASSERT_NE(c0, c1);
  ASSERT_EQ(client->send(c0, msg, sizeof(msg), false), 0);
  ASSERT_EQ(client->send(c1, msg, sizeof(msg), false), 0);

  char buf[64]{};
  size_t len = 0;
  uint32_t seen[2]{};
  size_t seen_count = 0;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline && seen_count < 2) {
    client->poll();
    server->poll();
    uint32_t in_cid = 0;
    if (server->recv(buf, sizeof(buf), &len, &in_cid) == 1) {
      if (seen_count == 0 || seen[0] != in_cid) {
        seen[seen_count++] = in_cid;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  EXPECT_EQ(seen_count, 2u);
  EXPECT_EQ(server->connection_stats().connected_peak, 2u);
  client->close();
  server->close();
}

TEST(CoopRudpAdapterSmoke, RecvRejectsNullConnIdWithoutConsuming) {
  auto server = rudp_bench::create_adapter("coop_rudp");
  auto client = rudp_bench::create_adapter("coop_rudp");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);
  server->server_listen(0xC408);

  const char msg[] = "null-output";
  uint32_t cid = client->client_connect("127.0.0.1", 0xC408);
  ASSERT_NE(cid, 0u);
  ASSERT_EQ(client->send(cid, msg, sizeof(msg), false), 0);

  char buf[64]{};
  size_t len = 0;
  uint32_t in_cid = 0;
  bool got = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    client->poll();
    server->poll();
    EXPECT_EQ(server->recv(buf, sizeof(buf), &len, nullptr), -1);
    if (server->recv(buf, sizeof(buf), &len, &in_cid) == 1) {
      EXPECT_EQ(std::memcmp(buf, msg, sizeof(msg)), 0);
      got = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  EXPECT_TRUE(got);
  client->close();
  server->close();
}

TEST(CoopRudpAdapterSmoke, ServerListenCanReplaceOpenSocket) {
  auto server = rudp_bench::create_adapter("coop_rudp");
  auto client = rudp_bench::create_adapter("coop_rudp");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);

  server->server_listen(0xC409);
  run_echo_once(*server, *client, true, 0xC40A, "relisten");
  client->close();
  server->close();
}

TEST(CoopRudpAdapterSmoke, ClientCloseReconnectUsesFreshConnectionId) {
  auto server = rudp_bench::create_adapter("coop_rudp");
  auto client = rudp_bench::create_adapter("coop_rudp");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);
  server->server_listen(0xC40E);

  const char first[] = "first";
  uint32_t c0 = client->client_connect("127.0.0.1", 0xC40E);
  ASSERT_NE(c0, 0u);
  ASSERT_EQ(client->send(c0, first, sizeof(first), false), 0);

  char buf[64]{};
  size_t len = 0;
  uint32_t in_cid = 0;
  bool got_first = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    client->poll();
    server->poll();
    if (server->recv(buf, sizeof(buf), &len, &in_cid) == 1) {
      got_first = in_cid == c0;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(got_first);

  client->close();
  const char second[] = "second";
  uint32_t c1 = client->client_connect("127.0.0.1", 0xC40E);
  ASSERT_NE(c1, 0u);
  EXPECT_NE(c1, c0);
  ASSERT_EQ(client->send(c1, second, sizeof(second), false), 0);

  bool got_second = false;
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    client->poll();
    server->poll();
    if (server->recv(buf, sizeof(buf), &len, &in_cid) == 1) {
      got_second = in_cid == c1 && std::memcmp(buf, second, sizeof(second)) == 0;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  EXPECT_TRUE(got_second);
  client->close();
  server->close();
}

TEST(CoopRudpAdapterSmoke, CloseAllowsReuseOnNewSocket) {
  auto server = rudp_bench::create_adapter("coop_rudp");
  auto client = rudp_bench::create_adapter("coop_rudp");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);

  run_echo_once(*server, *client, false, 0xC405, "reuse-unreliable");
  client->close();
  server->close();

  run_echo_once(*server, *client, true, 0xC406, "reuse-reliable");
  client->close();
  server->close();
}

TEST(CoopRudpAdapterSmoke, AsyncTxEchoUsesWorkerSendPath) {
  ScopedEnv env("COOP_ASYNC_TX", "1");
  auto server = rudp_bench::create_adapter("coop_rudp");
  auto client = rudp_bench::create_adapter("coop_rudp");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);

  run_echo_once(*server, *client, false, 0xC411, "async-worker");
  client->close();
  server->close();
}

TEST(CoopRudpAdapterSmoke, AsyncTxQueueLimitRequeuesUnacceptedPackets) {
  ScopedEnv async("COOP_ASYNC_TX", "1");
  ScopedEnv packet_limit("COOP_ASYNC_TX_PACKET_LIMIT", "1");
  ScopedEnv byte_limit("COOP_ASYNC_TX_BYTE_LIMIT", "65536");
  auto server = rudp_bench::create_adapter("coop_rudp");
  auto client = rudp_bench::create_adapter("coop_rudp");
  ASSERT_NE(server, nullptr);
  ASSERT_NE(client, nullptr);
  constexpr uint16_t kPort = 0xC414;
  server->server_listen(kPort);

  const char hello[] = "open-server-conn";
  uint32_t cid = client->client_connect("127.0.0.1", kPort);
  ASSERT_NE(cid, 0u);
  ASSERT_EQ(client->send(cid, hello, sizeof(hello), false), 0);

  char server_buf[64]{};
  size_t server_len = 0;
  uint32_t server_cid = 0;
  bool server_got = false;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    client->poll();
    server->poll();
    if (server->recv(server_buf, sizeof(server_buf), &server_len,
                     &server_cid) == 1) {
      server_got = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(server_got);
  ASSERT_EQ(server_cid, cid);

  std::vector<uint8_t> first(server->max_payload_bytes(false), 0x11);
  std::vector<uint8_t> second(server->max_payload_bytes(false), 0x22);
  first[0] = 1;
  second[0] = 2;
  ASSERT_EQ(server->send(cid, first.data(), first.size(), false), 0);
  ASSERT_EQ(server->send(cid, second.data(), second.size(), false), 0);
  server->poll();

  std::vector<uint8_t> buf(2048);
  size_t len = 0;
  uint32_t in_cid = 0;
  bool got_first = false;
  bool got_second = false;
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline && !got_first) {
    client->poll();
    while (client->recv(buf.data(), buf.size(), &len, &in_cid) == 1) {
      ASSERT_EQ(in_cid, cid);
      if (len == first.size() &&
          std::memcmp(buf.data(), first.data(), first.size()) == 0) {
        got_first = true;
      }
      if (len == second.size() &&
          std::memcmp(buf.data(), second.data(), second.size()) == 0) {
        got_second = true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(got_first);

  auto quiet_until = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(50);
  while (std::chrono::steady_clock::now() < quiet_until) {
    client->poll();
    while (client->recv(buf.data(), buf.size(), &len, &in_cid) == 1) {
      if (len == second.size() &&
          std::memcmp(buf.data(), second.data(), second.size()) == 0) {
        got_second = true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_FALSE(got_second);

  server->poll();
  deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline && !got_second) {
    client->poll();
    while (client->recv(buf.data(), buf.size(), &len, &in_cid) == 1) {
      ASSERT_EQ(in_cid, cid);
      if (len == second.size() &&
          std::memcmp(buf.data(), second.data(), second.size()) == 0) {
        got_second = true;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_TRUE(got_second);

  client->close();
  server->close();
}
