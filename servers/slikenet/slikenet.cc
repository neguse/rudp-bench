// SLikeNet(RakNet の後継)の層。LT は UNRELIABLE_SEQUENCED、MD は RELIABLE_ORDERED で送る(Source/include/slikenet/PacketPriority.h の PacketReliability の説明)。
// 互いに待たされないよう別の ordering channel に載せる(Help/RakNet/documentation/sendingpackets.html の「Ordering streams」。ordered と sequenced は別の組)。
// 優先度は HIGH_PRIORITY(同ディレクトリの tutorialsample3.html の Send の例)。
// 設定は既定のままで、受け付ける接続数だけ SetMaximumIncomingConnections で上げる(既定の 0 では接続を受けない。Source/include/slikenet/peerinterface.h)。
// 1 つの RakPeer は同じ相手に 1 本しか接続できないので、client は接続ごとに RakPeer を持つ。
//
// SLIKENET_TIMEOUT30 を定義すると、切断までの時間を既定の 10 秒から 30 秒にする(peerinterface.h の SetTimeoutTime)。
// トンネルやエレベーター、Wi-Fi と携帯回線の切り替えで回線が十数秒切れても、接続を保ちたいゲームで使う。
#include "bk_sut.h"

#include <slikenet/MessageIdentifiers.h>
#include <slikenet/peerinterface.h>
#include <slikenet/statistics.h>
#include <slikenet/types.h>

#ifdef SLIKENET_TIMEOUT30
#define NAME "slikenet-timeout30"
#define TIMEOUT_TUNING                                                  \
  ",{\"knob\":\"SetTimeoutTime\",\"value\":\"30000 ms\","                \
  "\"reason\":\"keep the session through outages of a few tens of seconds\"," \
  "\"upstream_ref\":\"Source/include/slikenet/peerinterface.h\"}"
#else
#define NAME "slikenet"
#define TIMEOUT_TUNING ""
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

constexpr unsigned char kChannelMd = 0;
constexpr unsigned char kChannelLt = 1;
// 利用者の最初のバイトはメッセージ種別でなければならない(peerinterface.h の Send)
constexpr unsigned char kMsgId = ID_USER_PACKET_ENUM;

struct sl_t {
  bk_sut *sut = nullptr;
  bool server = false;
  std::vector<SLNet::RakPeerInterface *> peers;  // server は 1 つ、client は接続ごと
  std::vector<SLNet::SystemAddress> addrs;       // 接続の番号 → 相手
};

SLNet::RakPeerInterface *peer_of(const sl_t *s, int conn) {
  return s->peers[s->server ? 0 : static_cast<size_t>(conn)];
}

int conn_index(const sl_t *s, size_t peer, const SLNet::SystemAddress &a) {
  if (!s->server) {
    return static_cast<int>(peer);
  }
  for (size_t i = 0; i < s->addrs.size(); ++i) {
    if (s->addrs[i] == a) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

sl_t *create(bk_sut *sut, bool server, size_t peers, int conns) {
  sl_t *s = new sl_t;
  s->sut = sut;
  s->server = server;
  s->addrs.assign(static_cast<size_t>(conns), SLNet::UNASSIGNED_SYSTEM_ADDRESS);
  // Linux では GUID が作った時刻のマイクロ秒なので(RakPeer.cpp の Get64BitUniqueRandomNumber)、同じマイクロ秒に作った RakPeer は GUID が重なり、server は 2 本目を ID_ALREADY_CONNECTED で断る。
  // 重ならなくなるまで作り直す。
  while (s->peers.size() < peers) {
    SLNet::RakPeerInterface *p = SLNet::RakPeerInterface::GetInstance();
    const bool dup = std::any_of(
        s->peers.begin(), s->peers.end(), [p](SLNet::RakPeerInterface *q) {
          return q->GetMyGUID() == p->GetMyGUID();
        });
    if (dup) {
      SLNet::RakPeerInterface::DestroyInstance(p);
    } else {
      s->peers.push_back(p);
    }
  }
  return s;
}

void sl_close(void *t) {
  sl_t *s = static_cast<sl_t *>(t);
  for (SLNet::RakPeerInterface *p : s->peers) {
    p->Shutdown(0);
    SLNet::RakPeerInterface::DestroyInstance(p);
  }
  delete s;
}

void *sl_listen(bk_sut *sut, uint16_t port, int max_conns) {
  sl_t *s = create(sut, true, 1, max_conns);
  SLNet::SocketDescriptor sd(port, nullptr);
  if (s->peers[0]->Startup(static_cast<unsigned>(max_conns), &sd, 1) !=
      SLNet::RAKNET_STARTED) {
    sl_close(s);
    return nullptr;
  }
  s->peers[0]->SetMaximumIncomingConnections(
      static_cast<unsigned short>(max_conns));
#ifdef SLIKENET_TIMEOUT30
  s->peers[0]->SetTimeoutTime(30000, SLNet::UNASSIGNED_SYSTEM_ADDRESS);
#endif
  return s;
}

void *sl_connect(bk_sut *sut, const char *host, uint16_t port, int conns) {
  sl_t *s = create(sut, false, static_cast<size_t>(conns), conns);
  bool ok = true;
  for (int i = 0; ok && i < conns; ++i) {
    SLNet::SocketDescriptor sd;
    ok = s->peers[static_cast<size_t>(i)]->Startup(1, &sd, 1) ==
             SLNet::RAKNET_STARTED;
#ifdef SLIKENET_TIMEOUT30
    s->peers[static_cast<size_t>(i)]->SetTimeoutTime(
        30000, SLNet::UNASSIGNED_SYSTEM_ADDRESS);
#endif
    ok = ok &&
         s->peers[static_cast<size_t>(i)]->Connect(host, port, nullptr, 0) ==
             SLNet::CONNECTION_ATTEMPT_STARTED;
  }
  if (!ok) {
    sl_close(s);
    return nullptr;
  }
  return s;
}

int sl_send(void *t, int conn, bool must_deliver, const void *data,
            size_t len) {
  sl_t *s = static_cast<sl_t *>(t);
  const char id = static_cast<char>(kMsgId);
  const char *parts[] = {&id, static_cast<const char *>(data)};
  const int lens[] = {1, static_cast<int>(len)};
  return peer_of(s, conn)->SendList(
             parts, lens, 2, HIGH_PRIORITY,
             must_deliver ? RELIABLE_ORDERED : UNRELIABLE_SEQUENCED,
             static_cast<char>(must_deliver ? kChannelMd : kChannelLt),
             s->addrs[static_cast<size_t>(conn)], false) != 0
             ? 0
             : -1;
}

// 戻り値は届いたメッセージ数
int drain(sl_t *s) {
  int total = 0;
  for (size_t i = 0; i < s->peers.size(); ++i) {
    SLNet::RakPeerInterface *peer = s->peers[i];
    while (SLNet::Packet *p = peer->Receive()) {
      const int conn = conn_index(s, i, p->systemAddress);
      switch (p->data[0]) {
        case ID_NEW_INCOMING_CONNECTION: {
          // server: 空いている番号を順に割り当てる
          auto it = std::find(s->addrs.begin(), s->addrs.end(),
                              SLNet::UNASSIGNED_SYSTEM_ADDRESS);
          if (it != s->addrs.end()) {
            *it = p->systemAddress;
            bk_sut_on_connect(s->sut, static_cast<int>(it - s->addrs.begin()));
          }
          break;
        }
        case ID_CONNECTION_REQUEST_ACCEPTED:
          s->addrs[i] = p->systemAddress;
          bk_sut_on_connect(s->sut, conn);
          break;
        case kMsgId:
          if (conn >= 0 && p->length > 1) {
            bk_sut_on_receive(s->sut, conn, p->data + 1, p->length - 1);
            ++total;
          }
          break;
        case ID_DISCONNECTION_NOTIFICATION:
        case ID_CONNECTION_LOST:
        case ID_CONNECTION_ATTEMPT_FAILED:
        case ID_NO_FREE_INCOMING_CONNECTIONS:
          if (conn >= 0) {
            std::fprintf(stderr, "slikenet: connection closed: message id %u\n",
                         p->data[0]);
            bk_sut_on_disconnect(s->sut, conn);
          }
          break;
        default:
          break;
      }
      peer->DeallocatePacket(p);
    }
  }
  return total;
}

// SLikeNet は待つ API を持たないので、1 ms ごとに見に行く
int sl_service(void *t, uint64_t timeout_ns) {
  sl_t *s = static_cast<sl_t *>(t);
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::nanoseconds(timeout_ns);
  for (;;) {
    const int n = drain(s);
    const auto now = std::chrono::steady_clock::now();
    if (n > 0 || now >= deadline) {
      return 0;
    }
    std::this_thread::sleep_for(
        std::min<std::chrono::nanoseconds>(deadline - now,
                                           std::chrono::milliseconds(1)));
  }
}

// まだ送っていない分(優先度ごとの送信待ち)と、確認応答を待つ reliable の再送待ち。
// 数えるのは利用者データの bytes で、ヘッダは含まない
// (ReliabilityLayer.cpp の statistics.bytesInSendBuffer / bytesInResendBuffer の加減算)。
int64_t sl_send_queue(void *t, int conn) {
  sl_t *s = static_cast<sl_t *>(t);
  SLNet::RakNetStatistics rns;
  if (peer_of(s, conn)->GetStatistics(s->addrs[static_cast<size_t>(conn)],
                                      &rns) == nullptr) {
    return -1;
  }
  double bytes = static_cast<double>(rns.bytesInResendBuffer);
  for (double b : rns.bytesInSendBuffer) {
    bytes += b;
  }
  return static_cast<int64_t>(bytes);
}

const bk_transport transport = [] {
  bk_transport t{};
  t.name = NAME;
  t.describe =
      "\"class_mapping\":{"
      "\"loss_tolerant\":{\"primitive\":\"UNRELIABLE_SEQUENCED (ordering channel 1)\","
      "\"delivery\":\"best_effort\",\"ordering\":\"ordered\","
      "\"realization\":\"native\"},"
      "\"must_deliver\":{\"primitive\":\"RELIABLE_ORDERED (ordering channel 0)\","
      "\"delivery\":\"reliable\",\"ordering\":\"ordered\","
      "\"realization\":\"native\"}},"
      "\"coalescing\":\"HIGH_PRIORITY messages are buffered and sent in groups at 10 ms intervals\","
      "\"cc_algo\":\"slikenet sliding window (default)\","
      "\"thread_model\":\"internal_worker\",\"encryption\":false,"
      "\"tuning\":[{\"knob\":\"SetMaximumIncomingConnections\","
      "\"value\":\"number of clients (server)\","
      "\"upstream_ref\":\"Source/include/slikenet/peerinterface.h\"}" TIMEOUT_TUNING "]";
  t.max_payload = 1200;
  t.listen = sl_listen;
  t.connect = sl_connect;
  t.send = sl_send;
  t.service = sl_service;
  t.send_queue = sl_send_queue;
  t.send_queue_unit = "bytes";
  t.close = sl_close;
  return t;
}();

}  // namespace

int main(int argc, char **argv) { return bk_sut_main(argc, argv, &transport); }
