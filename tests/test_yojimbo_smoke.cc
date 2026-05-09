#include <gtest/gtest.h>
#include "harness/adapter_registry.h"

#include <chrono>
#include <set>
#include <thread>
#include <vector>

namespace rudp_bench { void register_yojimbo_adapter(); }

class YojimboRegistrar {
public:
    YojimboRegistrar() { rudp_bench::register_yojimbo_adapter(); }
};
static YojimboRegistrar registrar;

using namespace rudp_bench;

// ポート 0xC106 = 49414
static const uint16_t kPort = 0xC106;

TEST(YojimboSmoke, Capability) {
    auto a = create_adapter("yojimbo");
    ASSERT_NE(a, nullptr);
    EXPECT_TRUE(a->supports(true));
    EXPECT_TRUE(a->supports(false));
    EXPECT_EQ(a->max_payload_bytes(true), 4096u);
    EXPECT_EQ(a->max_payload_bytes(false), 4096u);
    EXPECT_TRUE(a->encryption_on());   // yojimbo は暗号必須
    EXPECT_STREQ(a->name(), "yojimbo");
}

TEST(YojimboSmoke, ReliableEcho) {
    auto server = create_adapter("yojimbo");
    auto client = create_adapter("yojimbo");
    ASSERT_NE(server, nullptr);
    ASSERT_NE(client, nullptr);

    server->server_listen(kPort);

    std::thread server_thread([&]() {
        char buf[4096];
        size_t len;
        uint32_t cid;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            server->poll();
            if (server->recv(buf, sizeof(buf), &len, &cid) == 1) {
                server->send(cid, buf, len, /*reliable=*/true);
                // flush と echo が届くまで poll し続ける
                auto flush_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                while (std::chrono::steady_clock::now() < flush_deadline) {
                    server->poll();
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    uint32_t cid = client->client_connect("127.0.0.1", kPort);
    EXPECT_EQ(cid, 0u);

    // 接続完了まで待つ
    auto connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!client->is_connected(cid) &&
           std::chrono::steady_clock::now() < connect_deadline) {
        client->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(client->is_connected(cid));

    std::vector<uint8_t> oversized(client->max_payload_bytes(true) + 1);
    EXPECT_EQ(client->send(cid, oversized.data(), oversized.size(), /*reliable=*/true), -1);

    const char msg[] = "yojimbo-hello";
    EXPECT_EQ(client->send(cid, msg, sizeof(msg), /*reliable=*/true), 0);

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

TEST(YojimboSmoke, TwoClientConnectionsEcho) {
    auto server = create_adapter("yojimbo");
    auto client = create_adapter("yojimbo");
    ASSERT_NE(server, nullptr);
    ASSERT_NE(client, nullptr);

    server->server_listen(kPort + 1);
    std::set<uint32_t> server_conn_ids;

    std::thread server_thread([&]() {
        char buf[4096];
        size_t len;
        uint32_t cid;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
        while (std::chrono::steady_clock::now() < deadline && server_conn_ids.size() < 2) {
            server->poll();
            if (server->recv(buf, sizeof(buf), &len, &cid) == 1) {
                server_conn_ids.insert(cid);
                server->send(cid, buf, len, /*reliable=*/true);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        auto flush_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < flush_deadline) {
            server->poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    uint32_t c0 = client->client_connect("127.0.0.1", kPort + 1);
    uint32_t c1 = client->client_connect("127.0.0.1", kPort + 1);
    EXPECT_NE(c0, c1);

    auto connect_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while ((!client->is_connected(c0) || !client->is_connected(c1)) &&
           std::chrono::steady_clock::now() < connect_deadline) {
        client->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ASSERT_TRUE(client->is_connected(c0));
    ASSERT_TRUE(client->is_connected(c1));

    const char msg0[] = "yojimbo-c0";
    const char msg1[] = "yojimbo-c1";
    EXPECT_EQ(client->send(c0, msg0, sizeof(msg0), /*reliable=*/true), 0);
    EXPECT_EQ(client->send(c1, msg1, sizeof(msg1), /*reliable=*/true), 0);

    std::set<uint32_t> echoed_client_ids;
    char buf[4096];
    size_t len;
    uint32_t in_cid;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < deadline && echoed_client_ids.size() < 2) {
        client->poll();
        if (client->recv(buf, sizeof(buf), &len, &in_cid) == 1) {
            echoed_client_ids.insert(in_cid);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    server_thread.join();
    EXPECT_EQ(server_conn_ids.size(), 2u);
    EXPECT_EQ(echoed_client_ids.count(c0), 1u);
    EXPECT_EQ(echoed_client_ids.count(c1), 1u);

    client->close();
    server->close();
}
