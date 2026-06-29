#include <gtest/gtest.h>
#include "harness/adapter_registry.h"

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

namespace rudp_bench { void register_udt4_adapter(); }

class Udt4Registrar {
 public:
    Udt4Registrar() { rudp_bench::register_udt4_adapter(); }
};
static Udt4Registrar registrar;

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

TEST(Udt4Smoke, ReliableEcho) {
    auto server = create_adapter("udt4");
    auto client = create_adapter("udt4");
    ASSERT_NE(server, nullptr);
    ASSERT_NE(client, nullptr);

    // Port 0xC105 = 49413 (reserved for udt4 smoke test)
    server->server_listen(0xC105);

    std::thread server_thread([&]() {
        char buf[4096];
        size_t len;
        uint32_t cid;
        auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            server->poll();
            if (server->recv(buf, sizeof(buf), &len, &cid) == 1) {
                server->send(cid, buf, len, true);
                for (int i = 0; i < 30; ++i) {
                    server->poll();
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    // UDT::connect is synchronous so is_connected is immediately true.
    uint32_t cid = client->client_connect("127.0.0.1", 0xC105);
    ASSERT_TRUE(client->is_connected(cid));

    const char msg[] = "udt4-hello";
    EXPECT_EQ(client->send(cid, msg, sizeof(msg), true), 0);

    char buf[4096];
    size_t len;
    uint32_t in_cid;
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

TEST(Udt4Smoke, Capability) {
    auto a = create_adapter("udt4");
    ASSERT_NE(a, nullptr);
    // UDT4 supports reliable only; unreliable traffic is skipped by harness.
    EXPECT_TRUE(a->supports(true));
    EXPECT_FALSE(a->supports(false));
    EXPECT_FALSE(a->encryption_on());
    EXPECT_STREQ(a->name(), "udt4");
}

TEST(Udt4Smoke, PeerCloseMarksConnectionDisconnected) {
    auto server = create_adapter("udt4");
    auto client = create_adapter("udt4");
    ASSERT_NE(server, nullptr);
    ASSERT_NE(client, nullptr);

    constexpr uint16_t kPort = 0xC106;
    server->server_listen(kPort);
    uint32_t cid = client->client_connect("127.0.0.1", kPort);
    ASSERT_TRUE(client->is_connected(cid));

    for (int i = 0; i < 10; ++i) {
        server->poll();
        client->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    server->close();

    const char msg[] = "after-close";
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(7);
    while (std::chrono::steady_clock::now() < deadline &&
           client->is_connected(cid)) {
        client->poll();
        (void)client->send(cid, msg, sizeof(msg), true);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    EXPECT_FALSE(client->is_connected(cid));
    EXPECT_EQ(client->send(cid, msg, sizeof(msg), true), -1);
    EXPECT_EQ(client->connection_stats().shutdown_by_transport, 1u);

    client->close();
}

TEST(Udt4Smoke, OutPendingCapReturnsBackpressure) {
    ScopedEnv cap("UDT4_OUT_PENDING_BYTES", "8");
    auto server = create_adapter("udt4");
    auto client = create_adapter("udt4");
    ASSERT_NE(server, nullptr);
    ASSERT_NE(client, nullptr);

    constexpr uint16_t kPort = 0xC107;
    server->server_listen(kPort);
    uint32_t cid = client->client_connect("127.0.0.1", kPort);
    ASSERT_TRUE(client->is_connected(cid));

    const char msg[] = "too-large";
    EXPECT_EQ(client->send(cid, msg, sizeof(msg), true), -1);
    EXPECT_TRUE(client->is_connected(cid));

    client->close();
    server->close();
}
