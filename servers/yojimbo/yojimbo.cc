// yojimbo の層。LT は unreliable-unordered チャネル、MD は reliable-ordered チャネルに載せる(include/yojimbo_config.h の ChannelType と ChannelConfig の説明、USAGE.md の「reliable は頻度の低い重要なメッセージ、unreliable は毎フレーム送る状態」)。
// 接続は matchmaker を使わない insecure の経路にする(client.cpp / server.cpp の "insecure" の例)。
// チャネルの種類以外の設定は既定のまま。
//
// YOJIMBO_TIMEOUT30 を定義すると、切断までの時間を既定の 10 秒から 30 秒にする(include/yojimbo_config.h の ClientServerConfig::timeout)。
// トンネルやエレベーター、Wi-Fi と携帯回線の切り替えで回線が十数秒切れても、接続を保ちたいゲームで使う。
#include "bk_sut.h"

#include "benchkit.h"

#include <yojimbo.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {

using namespace yojimbo;

constexpr int kChannelMd = 0;
constexpr int kChannelLt = 1;
constexpr int kChannels = 2;
#ifdef YOJIMBO_TIMEOUT30
#define NAME "yojimbo-timeout30"
#define TUNING                                                              \
  "[{\"knob\":\"ClientServerConfig::timeout\",\"value\":\"30 s\","            \
  "\"reason\":\"keep the session through outages of a few tens of seconds\"," \
  "\"upstream_ref\":\"include/yojimbo_config.h\"}]"
constexpr int kTimeoutS = 30;
#else
#define NAME "yojimbo"
#define TUNING "[]"
constexpr int kTimeoutS = YOJIMBO_DEFAULT_TIMEOUT;
#endif
constexpr int kMaxPayload = 1200;
// サンプル(client.cpp / server.cpp)の SendPackets と ReceivePackets の周期
constexpr uint64_t kPumpNs = 10 * 1000 * 1000;

struct BenchMessage : public Message {
  int length = 0;
  uint8_t data[kMaxPayload];

  template <typename Stream>
  bool Serialize(Stream &stream) {
    serialize_int(stream, length, 0, kMaxPayload);
    serialize_bytes(stream, data, length);
    return true;
  }

  YOJIMBO_VIRTUAL_SERIALIZE_FUNCTIONS()
};

enum { kMessageBench, kNumMessageTypes };

YOJIMBO_MESSAGE_FACTORY_START(BenchMessageFactory, kNumMessageTypes);
YOJIMBO_DECLARE_MESSAGE_TYPE(kMessageBench, BenchMessage);
YOJIMBO_MESSAGE_FACTORY_FINISH();

struct yojimbo_t : public Adapter {
  bk_sut *sut = nullptr;
  ClientServerConfig config;
  Server *server = nullptr;
  std::vector<Client *> clients;
  std::vector<bool> up;    // client: on_connect を出したか
  std::vector<bool> gone;  // client: on_disconnect を出したか(切れた後も状態は ERROR のまま残る)
  uint64_t last_send_ns = 0;

  MessageFactory *CreateMessageFactory(Allocator &allocator) override {
    return YOJIMBO_NEW(allocator, BenchMessageFactory, allocator);
  }
  void OnServerClientConnected(int i) override { bk_sut_on_connect(sut, i); }
  void OnServerClientDisconnected(int i) override {
    std::fprintf(stderr, "yojimbo: client %d disconnected: %s\n", i,
                 GetServerClientDisconnectReasonString(
                     server->GetClientDisconnectReason(i)));
    bk_sut_on_disconnect(sut, i);
  }
};

double now_s() { return static_cast<double>(bk_now_ns()) * 1e-9; }

yojimbo_t *create(bk_sut *sut) {
  if (!InitializeYojimbo()) {
    return nullptr;
  }
  yojimbo_t *y = new yojimbo_t;
  y->sut = sut;
  y->config.channel[kChannelLt].type = CHANNEL_TYPE_UNRELIABLE_UNORDERED;
  y->config.timeout = kTimeoutS;
  return y;
}

void yojimbo_close(void *t) {
  yojimbo_t *y = static_cast<yojimbo_t *>(t);
  if (y->server != nullptr) {
    y->server->Stop();
    delete y->server;
  }
  for (Client *c : y->clients) {
    c->Disconnect();
    delete c;
  }
  delete y;
  ShutdownYojimbo();
}

void *yojimbo_listen(bk_sut *sut, uint16_t port, int max_conns) {
  yojimbo_t *y = create(sut);
  if (y == nullptr) {
    return nullptr;
  }
  // 接続トークンのサーバアドレスと一致しない要求は拒否されるので、client が接続する先を渡す
  const char *host = bk_sut_host(sut);
  uint8_t key[KeyBytes] = {};
  y->server = new Server(GetDefaultAllocator(), key,
                         Address(host != nullptr ? host : "127.0.0.1", port),
                         y->config, *y, now_s());
  y->server->Start(max_conns);
  if (!y->server->IsRunning()) {
    yojimbo_close(y);
    return nullptr;
  }
  return y;
}

void *yojimbo_connect(bk_sut *sut, const char *host, uint16_t port, int conns) {
  yojimbo_t *y = create(sut);
  if (y == nullptr) {
    return nullptr;
  }
  uint8_t key[KeyBytes] = {};
  for (int i = 0; i < conns; ++i) {
    uint64_t id = 0;
    yojimbo_random_bytes(reinterpret_cast<uint8_t *>(&id), sizeof(id));
    Client *c = new Client(GetDefaultAllocator(), Address("0.0.0.0"), y->config,
                           *y, now_s());
    y->clients.push_back(c);
    c->InsecureConnect(key, id, Address(host, port));
  }
  y->up.assign(static_cast<size_t>(conns), false);
  y->gone.assign(static_cast<size_t>(conns), false);
  return y;
}

int yojimbo_send(void *t, int conn, bool must_deliver, const void *data,
                 size_t len) {
  yojimbo_t *y = static_cast<yojimbo_t *>(t);
  const int ch = must_deliver ? kChannelMd : kChannelLt;
  const bool server = y->server != nullptr;
  // キューが満杯のまま SendMessage すると接続がエラーで切れるので、渡さずに返す
  if (!(server ? y->server->CanSendMessage(conn, ch)
               : y->clients[conn]->CanSendMessage(ch))) {
    return -1;
  }
  Message *m = server ? y->server->CreateMessage(conn, kMessageBench)
                      : y->clients[conn]->CreateMessage(kMessageBench);
  if (m == nullptr) {
    return -1;
  }
  BenchMessage *b = static_cast<BenchMessage *>(m);
  b->length = static_cast<int>(len);
  std::memcpy(b->data, data, len);
  if (server) {
    y->server->SendMessage(conn, ch, m);
  } else {
    y->clients[conn]->SendMessage(ch, m);
  }
  return 0;
}

void yojimbo_flush(void *t) {
  yojimbo_t *y = static_cast<yojimbo_t *>(t);
  y->last_send_ns = bk_now_ns();
  if (y->server != nullptr) {
    y->server->SendPackets();
  }
  for (Client *c : y->clients) {
    c->SendPackets();
  }
}

void deliver(yojimbo_t *y, int conn, Message *m) {
  const BenchMessage *b = static_cast<const BenchMessage *>(m);
  bk_sut_on_receive(y->sut, conn, b->data, static_cast<size_t>(b->length));
}

void pump(yojimbo_t *y) {
  const double now = now_s();
  if (y->server != nullptr) {
    y->server->ReceivePackets();
    for (int i = 0; i < y->server->GetMaxClients(); ++i) {
      if (!y->server->IsClientConnected(i)) {
        continue;
      }
      for (int ch = 0; ch < kChannels; ++ch) {
        while (Message *m = y->server->ReceiveMessage(i, ch)) {
          deliver(y, i, m);
          y->server->ReleaseMessage(i, m);
        }
      }
    }
    y->server->AdvanceTime(now);
  }
  for (size_t i = 0; i < y->clients.size(); ++i) {
    Client *c = y->clients[i];
    const int idx = static_cast<int>(i);
    c->ReceivePackets();
    // 切れた Client は Connection を解放しているので(yojimbo_client.cpp の Client::Disconnect)、ReceiveMessage を呼べない。
    for (int ch = 0; ch < kChannels && c->IsConnected(); ++ch) {
      while (Message *m = c->ReceiveMessage(ch)) {
        deliver(y, idx, m);
        c->ReleaseMessage(m);
      }
    }
    c->AdvanceTime(now);
    if (c->IsConnected() && !y->up[i]) {
      y->up[i] = true;
      bk_sut_on_connect(y->sut, idx);
    } else if (!y->gone[i] && (c->GetClientState() == CLIENT_STATE_ERROR ||
                               (y->up[i] && c->IsDisconnected()))) {
      std::fprintf(stderr, "yojimbo: client %d disconnected: %s\n", idx,
                   GetClientDisconnectReasonString(c->GetDisconnectReason()));
      y->gone[i] = true;
      bk_sut_on_disconnect(y->sut, idx);
    }
  }
}

// yojimbo は待つ API を持たないので、1 ms ごとに見に行く。
// ack と再送を進めるため、送るものがなくても 10 ms ごとに SendPackets する。
int yojimbo_service(void *t, uint64_t timeout_ns) {
  yojimbo_t *y = static_cast<yojimbo_t *>(t);
  const uint64_t deadline = bk_now_ns() + timeout_ns;
  for (;;) {
    if (bk_now_ns() - y->last_send_ns >= kPumpNs) {
      yojimbo_flush(y);
    }
    pump(y);
    const uint64_t now = bk_now_ns();
    if (now >= deadline) {
      return 0;
    }
    std::this_thread::sleep_for(std::chrono::nanoseconds(
        std::min<uint64_t>(deadline - now, 1000 * 1000)));
  }
}

const bk_transport transport = [] {
  bk_transport t{};
  t.name = NAME;
  t.describe =
      "\"class_mapping\":{"
      "\"loss_tolerant\":{\"primitive\":\"unreliable-unordered channel\","
      "\"delivery\":\"best_effort\",\"ordering\":\"unordered\","
      "\"realization\":\"native\"},"
      "\"must_deliver\":{\"primitive\":\"reliable-ordered channel\","
      "\"delivery\":\"reliable\",\"ordering\":\"ordered\","
      "\"realization\":\"native\"}},"
      "\"coalescing\":\"one packet per SendPackets, flushed at the end of each tick\","
      "\"cc_algo\":\"none\","
      "\"thread_model\":\"single\",\"encryption\":true,"
      "\"tuning\":" TUNING;
  t.max_payload = kMaxPayload;
  t.listen = yojimbo_listen;
  t.connect = yojimbo_connect;
  t.send = yojimbo_send;
  t.flush = yojimbo_flush;
  t.service = yojimbo_service;
  t.send_queue = nullptr;
  t.close = yojimbo_close;
  return t;
}();

}  // namespace

int main(int argc, char **argv) { return bk_sut_main(argc, argv, &transport); }
