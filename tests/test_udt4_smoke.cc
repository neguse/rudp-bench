#include <gtest/gtest.h>
#include "harness/adapter_registry.h"

#include <chrono>
#include <thread>

namespace rudp_bench { void register_udt4_adapter(); }

class Udt4Registrar {
 public:
    Udt4Registrar() { rudp_bench::register_udt4_adapter(); }
};
static Udt4Registrar registrar;

using namespace rudp_bench;

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
