// GameNetworkingSockets の層。LT は unreliable、MD は reliable で送り、tick の
// 終わりに FlushMessagesOnConnection で Nagle の待ちを切る
// (steamnetworkingtypes.h の k_nSteamNetworkingSend_NoNagle の説明にある使い方)。
// 設定は既定のまま(暗号も既定で有効)。
#include "bk_sut.h"

#include <steam/isteamnetworkingutils.h>
#include <steam/steamnetworkingsockets.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

constexpr int kRecvBatch = 256;

struct gns_t {
  bk_sut *sut = nullptr;
  bool server = false;
  ISteamNetworkingSockets *iface = nullptr;
  HSteamListenSocket listen = k_HSteamListenSocket_Invalid;
  HSteamNetPollGroup group = k_HSteamNetPollGroup_Invalid;
  std::vector<HSteamNetConnection> conns;  // 接続の番号 → handle
};

// status callback は利用者のポインタを取れないので、1 プロセスに 1 つの gns_t を置く
gns_t *g_gns = nullptr;

int conn_index(const gns_t *g, HSteamNetConnection h) {
  for (size_t i = 0; i < g->conns.size(); ++i) {
    if (g->conns[i] == h) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void on_status(SteamNetConnectionStatusChangedCallback_t *info) {
  gns_t *g = g_gns;
  const HSteamNetConnection h = info->m_hConn;
  switch (info->m_info.m_eState) {
    case k_ESteamNetworkingConnectionState_Connecting: {
      if (!g->server) {
        break;
      }
      const int i = conn_index(g, k_HSteamNetConnection_Invalid);
      if (i < 0 || g->iface->AcceptConnection(h) != k_EResultOK) {
        g->iface->CloseConnection(h, 0, nullptr, false);
        break;
      }
      g->conns[i] = h;
      g->iface->SetConnectionPollGroup(h, g->group);
      break;
    }
    case k_ESteamNetworkingConnectionState_Connected: {
      const int i = conn_index(g, h);
      if (i >= 0) {
        bk_sut_on_connect(g->sut, i);
      }
      break;
    }
    case k_ESteamNetworkingConnectionState_ClosedByPeer:
    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
      std::fprintf(stderr, "gns: connection closed: %s\n",
                   info->m_info.m_szEndDebug);
      const int i = conn_index(g, h);
      if (i >= 0) {
        bk_sut_on_disconnect(g->sut, i);
      }
      g->iface->CloseConnection(h, 0, nullptr, false);
      break;
    }
    default:
      break;
  }
}

gns_t *create(bk_sut *sut, bool server, int conns) {
  SteamNetworkingErrMsg err;
  if (g_gns != nullptr || !GameNetworkingSockets_Init(nullptr, err)) {
    return nullptr;
  }
  SteamNetworkingUtils()->SetGlobalCallback_SteamNetConnectionStatusChanged(
      on_status);
  g_gns = new gns_t;
  g_gns->sut = sut;
  g_gns->server = server;
  g_gns->iface = SteamNetworkingSockets();
  g_gns->group = g_gns->iface->CreatePollGroup();
  g_gns->conns.assign(static_cast<size_t>(conns), k_HSteamNetConnection_Invalid);
  return g_gns;
}

void gns_close(void *t) {
  gns_t *g = static_cast<gns_t *>(t);
  for (HSteamNetConnection h : g->conns) {
    if (h != k_HSteamNetConnection_Invalid) {
      g->iface->CloseConnection(h, 0, nullptr, false);
    }
  }
  if (g->listen != k_HSteamListenSocket_Invalid) {
    g->iface->CloseListenSocket(g->listen);
  }
  g->iface->DestroyPollGroup(g->group);
  delete g;
  g_gns = nullptr;
  GameNetworkingSockets_Kill();
}

void *gns_listen(bk_sut *sut, uint16_t port, int max_conns) {
  gns_t *g = create(sut, true, max_conns);
  if (g == nullptr) {
    return nullptr;
  }
  SteamNetworkingIPAddr addr;
  addr.Clear();
  addr.m_port = port;
  g->listen = g->iface->CreateListenSocketIP(addr, 0, nullptr);
  if (g->listen == k_HSteamListenSocket_Invalid) {
    gns_close(g);
    return nullptr;
  }
  return g;
}

void *gns_connect(bk_sut *sut, const char *host, uint16_t port, int conns) {
  gns_t *g = create(sut, false, conns);
  if (g == nullptr) {
    return nullptr;
  }
  SteamNetworkingIPAddr addr;
  bool ok = addr.ParseString(host);
  addr.m_port = port;
  for (int i = 0; ok && i < conns; ++i) {
    g->conns[i] = g->iface->ConnectByIPAddress(addr, 0, nullptr);
    ok = g->conns[i] != k_HSteamNetConnection_Invalid &&
         g->iface->SetConnectionPollGroup(g->conns[i], g->group);
  }
  if (!ok) {
    gns_close(g);
    return nullptr;
  }
  return g;
}

int gns_send(void *t, int conn, bool must_deliver, const void *data,
             size_t len) {
  gns_t *g = static_cast<gns_t *>(t);
  const EResult r = g->iface->SendMessageToConnection(
      g->conns[conn], data, static_cast<uint32>(len),
      must_deliver ? k_nSteamNetworkingSend_Reliable
                   : k_nSteamNetworkingSend_Unreliable,
      nullptr);
  return r == k_EResultOK ? 0 : -1;
}

void gns_flush(void *t) {
  gns_t *g = static_cast<gns_t *>(t);
  for (HSteamNetConnection h : g->conns) {
    if (h != k_HSteamNetConnection_Invalid) {
      g->iface->FlushMessagesOnConnection(h);
    }
  }
}

int drain(gns_t *g) {
  int total = 0;
  SteamNetworkingMessage_t *msgs[kRecvBatch];
  for (;;) {
    const int n = g->iface->ReceiveMessagesOnPollGroup(g->group, msgs,
                                                       kRecvBatch);
    if (n < 0) {
      return -1;
    }
    for (int i = 0; i < n; ++i) {
      bk_sut_on_receive(g->sut, conn_index(g, msgs[i]->m_conn),
                        msgs[i]->m_pData, static_cast<size_t>(msgs[i]->m_cbSize));
      msgs[i]->Release();
    }
    total += n;
    if (n < kRecvBatch) {
      return total;
    }
  }
}

// GNS は待つ API を持たないので、1 ms ごとに見に行く
int gns_service(void *t, uint64_t timeout_ns) {
  gns_t *g = static_cast<gns_t *>(t);
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::nanoseconds(timeout_ns);
  for (;;) {
    g->iface->RunCallbacks();
    const int n = drain(g);
    if (n < 0) {
      return -1;
    }
    const auto now = std::chrono::steady_clock::now();
    if (n > 0 || now >= deadline) {
      return 0;
    }
    std::this_thread::sleep_for(
        std::min<std::chrono::nanoseconds>(deadline - now,
                                           std::chrono::milliseconds(1)));
  }
}

int64_t gns_send_queue(void *t, int conn) {
  gns_t *g = static_cast<gns_t *>(t);
  SteamNetConnectionRealTimeStatus_t status;
  if (g->iface->GetConnectionRealTimeStatus(g->conns[conn], &status, 0,
                                            nullptr) != k_EResultOK) {
    return -1;
  }
  return status.m_cbPendingReliable + status.m_cbPendingUnreliable;
}

const bk_transport transport = [] {
  bk_transport t{};
  t.name = "gns";
  t.describe =
      "\"class_mapping\":{"
      "\"loss_tolerant\":{\"primitive\":\"unreliable\","
      "\"delivery\":\"best_effort\",\"ordering\":\"unordered\","
      "\"realization\":\"native\"},"
      "\"must_deliver\":{\"primitive\":\"reliable\","
      "\"delivery\":\"reliable\",\"ordering\":\"ordered\","
      "\"realization\":\"native\"}},"
      "\"coalescing\":\"nagle, flushed at the end of each tick\","
      "\"cc_algo\":\"gns default (SendRateMin/Max 256KB/s)\","
      "\"thread_model\":\"internal_worker\",\"encryption\":true,"
      "\"tuning\":[]";
  t.max_payload = 1200;
  t.listen = gns_listen;
  t.connect = gns_connect;
  t.send = gns_send;
  t.flush = gns_flush;
  t.service = gns_service;
  t.send_queue = gns_send_queue;
  t.send_queue_unit = "bytes";
  t.close = gns_close;
  return t;
}();

}  // namespace

int main(int argc, char **argv) { return bk_sut_main(argc, argv, &transport); }
