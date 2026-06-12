// yojimbo adapter for rudp-bench.
// Glenn Fiedler の netcode+reliable+serialize+sodium スタック。
// 暗号は必須(InsecureConnect を使っても libsodium による認証は有効)。
// server/client ともに同じ BenchAdapter + BenchMessageFactory を使う。
// conn_id: サーバ側 = clientIndex(0..MaxClients-1)、クライアント側 = client instance id。
// チャネル: 0=reliable-ordered、1=unreliable-unordered。

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "yojimbo.h"
#pragma GCC diagnostic pop

#include "harness/adapter.h"
#include "harness/adapter_registry.h"
#include "harness/inbound_queue.h"

#include <chrono>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

// InitializeYojimbo/ShutdownYojimbo はプロセス内で各1回のみ呼ぶ。
// 複数の adapter インスタンスが共存するため once_flag で制御する。
void ensure_yojimbo_init() {
    static std::once_flag flag;
    std::call_once(flag, []() {
        if (!InitializeYojimbo()) std::abort();
        yojimbo_log_level(YOJIMBO_LOG_LEVEL_NONE);
        std::atexit([]() { ShutdownYojimbo(); });
    });
}

// ベンチマーク専用プロトコル ID
static const uint64_t kProtocolId = 0x72756470626368ULL;  // "rudpbch"

// InsecureConnect 用の共有プライベートキー(テスト用途のみ、本番禁止)
static const uint8_t kTestPrivateKey[yojimbo::KeyBytes] = {
    0x60, 0x6f, 0x62, 0x6a, 0x69, 0x6d, 0x62, 0x6f,  // "objimbo" + zeros
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0
};

// BenchMessage が運べる最大ペイロードサイズ。
// デフォルト maxPacketSize=8KB に収まる上限。
static const int kMaxPayloadBytes = 4096;

// ----- メッセージ定義 -----

enum BenchMessageType {
    BENCH_MESSAGE = 0,
    NUM_BENCH_MESSAGE_TYPES
};

struct BenchMessage : public yojimbo::Message {
    uint32_t length = 0;
    uint8_t data[kMaxPayloadBytes];

    BenchMessage() { std::memset(data, 0, sizeof(data)); }

    template <typename Stream>
    bool Serialize(Stream& stream) {
        serialize_int(stream, length, 0, kMaxPayloadBytes);
        if (length > 0) {
            serialize_bytes(stream, data, (int)length);
        }
        return true;
    }

    YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS()
};

YOJIMBO_MESSAGE_FACTORY_START(BenchMessageFactory, NUM_BENCH_MESSAGE_TYPES)
YOJIMBO_DECLARE_MESSAGE_TYPE(BENCH_MESSAGE, BenchMessage)
YOJIMBO_MESSAGE_FACTORY_FINISH()

// yojimbo::Adapter サブクラス: MessageFactory 生成を担う
class BenchYojimboAdapter : public yojimbo::Adapter {
public:
    yojimbo::MessageFactory* CreateMessageFactory(yojimbo::Allocator& allocator) override {
        return YOJIMBO_NEW(allocator, BenchMessageFactory, allocator);
    }
};

// ----- 共通設定ファクトリ -----

static yojimbo::ClientServerConfig MakeConfig() {
    yojimbo::ClientServerConfig cfg;
    cfg.protocolId = kProtocolId;
    cfg.timeout = 10;
    cfg.numChannels = 2;
    cfg.channel[0].type = yojimbo::CHANNEL_TYPE_RELIABLE_ORDERED;
    cfg.channel[0].messageSendQueueSize = 4096;
    cfg.channel[0].messageReceiveQueueSize = 4096;
    cfg.channel[1].type = yojimbo::CHANNEL_TYPE_UNRELIABLE_UNORDERED;
    cfg.channel[1].messageSendQueueSize = 4096;
    cfg.channel[1].messageReceiveQueueSize = 4096;
    cfg.networkSimulator = false;  // loopback ベンチでは不要
    return cfg;
}

// ----- Adapter 実装 -----

class YojimboAdapter : public rudp_bench::Adapter {
public:
    YojimboAdapter() {
        ensure_yojimbo_init();
        start_ = std::chrono::steady_clock::now();
        time_ = 100.0;  // yojimbo examples start at 100.0 で安定
        config_ = MakeConfig();
    }

    ~YojimboAdapter() override {
        close();
        // ShutdownYojimbo は atexit で呼ぶため、ここでは呼ばない
    }

    void server_listen(uint16_t port) override {
        is_server_ = true;
        // netcode は connect token 内のサーバアドレスとサーバの bound address を照合する。
        // 0.0.0.0 では一致しないため 127.0.0.1 に明示的にバインドする(loopback 専用)。
        yojimbo::Address addr("127.0.0.1", port);
        server_ = std::make_unique<yojimbo::Server>(
            yojimbo::GetDefaultAllocator(),
            kTestPrivateKey,
            addr,
            config_,
            yojimbo_adapter_,
            time_
        );
        server_->Start(yojimbo::MaxClients);
    }

    uint32_t client_connect(const char* host, uint16_t port) override {
        is_server_ = false;
        uint32_t id = next_client_id_++;
        yojimbo::Address bind_addr("0.0.0.0");
        auto client = std::make_unique<yojimbo::Client>(
            yojimbo::GetDefaultAllocator(),
            bind_addr,
            config_,
            yojimbo_adapter_,
            time_
        );
        uint64_t client_id = 0;
        yojimbo_random_bytes(reinterpret_cast<uint8_t*>(&client_id), 8);
        yojimbo::Address server_addr(host, port);
        client->InsecureConnect(kTestPrivateKey, client_id, server_addr);
        clients_[id] = std::move(client);
        return id;
    }

    bool is_connected(uint32_t conn_id) override {
        if (is_server_) {
            return server_ && server_->IsClientConnected((int)conn_id);
        }
        auto it = clients_.find(conn_id);
        return it != clients_.end() && it->second->IsConnected();
    }

    int send(uint32_t conn_id, const void* data, size_t len, bool reliable) override {
        if (len > max_payload_bytes(reliable)) return -1;
        int channel = reliable ? 0 : 1;
        uint32_t actual_len = (uint32_t)len;

        if (is_server_) {
            if (!server_) return -1;
            int ci = (int)conn_id;
            if (!server_->IsClientConnected(ci)) return -1;
            if (!server_->CanSendMessage(ci, channel)) return -1;
            auto* msg = static_cast<BenchMessage*>(server_->CreateMessage(ci, BENCH_MESSAGE));
            if (!msg) return -1;
            msg->length = actual_len;
            std::memcpy(msg->data, data, actual_len);
            server_->SendMessage(ci, channel, msg);
        } else {
            auto it = clients_.find(conn_id);
            if (it == clients_.end()) return -1;
            auto* client = it->second.get();
            if (!client->IsConnected()) return -1;
            if (!client->CanSendMessage(channel)) return -1;
            auto* msg = static_cast<BenchMessage*>(client->CreateMessage(BENCH_MESSAGE));
            if (!msg) return -1;
            msg->length = actual_len;
            std::memcpy(msg->data, data, actual_len);
            client->SendMessage(channel, msg);
        }
        return 0;
    }

    int recv(void* buf, size_t cap, size_t* out_len, uint32_t* out_conn_id) override {
        return inbox_.recv(buf, cap, out_len, out_conn_id);
    }

    void poll() override {
        auto now = std::chrono::steady_clock::now();
        time_ = 100.0 + std::chrono::duration<double>(now - start_).count();

        // yojimbo は SendPackets() 呼び出しごとに必ず 1 パケット(サーバは
        // 接続クライアントごとに 1 パケット)を生成する固定ティック設計。
        // spin する harness の poll 周波数のまま呼ぶと数万 pkt/s のほぼ空
        // パケット洪水になり、受信側の復号がコア飽和 → rx ドロップで
        // delivery が c1 ですら 0.42 に崩壊していた。examples と同様に
        // 送信だけ固定 cadence(100Hz)に間引く。受信と時刻進行は毎 poll。
        const bool do_send = now >= next_send_packets_;
        if (do_send) {
            next_send_packets_ = now + std::chrono::milliseconds(10);
        }

        // AdvanceTime(タイムアウト/ack スケジューラ管理)も spin 周波数では
        // 全接続走査がコストになるため送信と同じ 100Hz に間引く。
        // ReceivePackets と message drain は受信レイテンシに効くので毎 poll。
        if (is_server_ && server_) {
            if (do_send) server_->SendPackets();
            server_->ReceivePackets();
            if (do_send) server_->AdvanceTime(time_);
            drain_server_messages();
        } else if (!is_server_) {
            for (auto& [id, client] : clients_) {
                if (do_send) client->SendPackets();
                client->ReceivePackets();
                if (do_send) client->AdvanceTime(time_);
                drain_client_messages(id, client.get());
            }
        }
    }

    void close() override {
        if (server_) { server_->Stop(); server_.reset(); }
        for (auto& [id, client] : clients_) client->Disconnect();
        clients_.clear();
    }

    const char* name() const override { return "yojimbo"; }
    bool supports(bool /*reliable*/) const override { return true; }
    size_t max_payload_bytes(bool /*reliable*/) const override { return kMaxPayloadBytes; }
    uint32_t max_connections() const override { return yojimbo::MaxClients; }
    const char* flush_policy(bool /*reliable*/) const override { return "poll_send_packets"; }
    bool encryption_on() const override { return true; }

private:
    void drain_server_messages() {
        for (int ci = 0; ci < server_->GetMaxClients(); ++ci) {
            if (!server_->IsClientConnected(ci)) continue;
            for (int ch = 0; ch < 2; ++ch) {
                yojimbo::Message* raw;
                while ((raw = server_->ReceiveMessage(ci, ch)) != nullptr) {
                    auto* m = static_cast<BenchMessage*>(raw);
                    inbox_.enqueue((uint32_t)ci, m->data, m->length);
                    server_->ReleaseMessage(ci, raw);
                }
            }
        }
    }

    void drain_client_messages(uint32_t conn_id, yojimbo::Client* client) {
        for (int ch = 0; ch < 2; ++ch) {
            yojimbo::Message* raw;
            while ((raw = client->ReceiveMessage(ch)) != nullptr) {
                auto* m = static_cast<BenchMessage*>(raw);
                inbox_.enqueue(conn_id, m->data, m->length);
                client->ReleaseMessage(raw);
            }
        }
    }

    BenchYojimboAdapter yojimbo_adapter_;
    yojimbo::ClientServerConfig config_;

    std::unique_ptr<yojimbo::Server> server_;
    std::unordered_map<uint32_t, std::unique_ptr<yojimbo::Client>> clients_;
    uint32_t next_client_id_ = 0;

    bool is_server_ = true;
    std::chrono::steady_clock::time_point start_;
    std::chrono::steady_clock::time_point next_send_packets_{};  // 100Hz 送信 cadence
    double time_ = 0.0;

    rudp_bench::ReusableInboundQueue inbox_;
};

}  // namespace

namespace rudp_bench {
void register_yojimbo_adapter() {
    register_adapter("yojimbo",
        []() { return std::make_unique<YojimboAdapter>(); });
}
}  // namespace rudp_bench
