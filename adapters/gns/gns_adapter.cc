#include "harness/adapter.h"
#include "harness/adapter_registry.h"
#include "harness/inbound_queue.h"

#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

#include <arpa/inet.h>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr int GNS_RECV_BATCH = 64;

// Forward declaration — defined after GnsAdapter.
static void gns_status_callback(SteamNetConnectionStatusChangedCallback_t* info);

// Global registry: routes status callbacks to the owning adapter instance.
// GNS fires callbacks synchronously from RunCallbacks(), so we hold g_gns.mtx
// only briefly to look up the adapter, then release before dispatching.
struct GnsGlobal {
  std::mutex mtx;
  std::unordered_map<HSteamListenSocket, class GnsAdapter*> listen_map;
  std::unordered_map<HSteamNetConnection, class GnsAdapter*> conn_map;
};
static GnsGlobal g_gns;

void ensure_gns_init() {
  static std::once_flag flag;
  std::call_once(flag, []() {
    SteamNetworkingErrMsg err;
    if (!GameNetworkingSockets_Init(nullptr, err)) {
      std::abort();
    }
    // GNS デバッグ出力をベンチ用に抑制する
    SteamNetworkingUtils()->SetDebugOutputFunction(
        k_ESteamNetworkingSocketsDebugOutputType_None, nullptr);
    std::atexit([]() { GameNetworkingSockets_Kill(); });
  });
}

class GnsAdapter : public rudp_bench::Adapter {
 public:
  GnsAdapter() {
    ensure_gns_init();
    iface_ = SteamNetworkingSockets();
  }

  ~GnsAdapter() override { close(); }

  void server_listen(uint16_t port) override {
    SteamNetworkingIPAddr addr;
    addr.Clear();
    addr.m_port = port;

    SteamNetworkingConfigValue_t opt;
    opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
               reinterpret_cast<void*>(gns_status_callback));

    listen_sock_ = iface_->CreateListenSocketIP(addr, 1, &opt);
    if (listen_sock_ == k_HSteamListenSocket_Invalid) std::abort();

    std::lock_guard<std::mutex> glock(g_gns.mtx);
    g_gns.listen_map[listen_sock_] = this;
  }

  uint32_t client_connect(const char* host, uint16_t port) override {
    SteamNetworkingIPAddr addr;
    addr.Clear();
    struct in_addr in{};
    inet_pton(AF_INET, host, &in);
    addr.SetIPv4(ntohl(in.s_addr), port);

    SteamNetworkingConfigValue_t opt;
    opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
               reinterpret_cast<void*>(gns_status_callback));

    HSteamNetConnection hConn = iface_->ConnectByIPAddress(addr, 1, &opt);
    if (hConn == k_HSteamNetConnection_Invalid) std::abort();

    // クライアント側: ConnectByIPAddress 直後に登録することで
    // 後続の Connecting コールバックで AcceptConnection を誤発行しない
    uint32_t id;
    {
      std::lock_guard<std::mutex> lock(mtx_);
      id = next_id_++;
      id_to_conn_[id] = hConn;
      conn_to_id_[hConn] = id;
    }
    {
      std::lock_guard<std::mutex> glock(g_gns.mtx);
      g_gns.conn_map[hConn] = this;
    }
    return id;
  }

  bool is_connected(uint32_t conn_id) override {
    std::lock_guard<std::mutex> lock(mtx_);
    return connected_ids_.count(conn_id) > 0;
  }

  int send(uint32_t conn_id, const void* data, size_t len, bool reliable) override {
    HSteamNetConnection hConn = k_HSteamNetConnection_Invalid;
    {
      std::lock_guard<std::mutex> lock(mtx_);
      auto it = id_to_conn_.find(conn_id);
      if (it == id_to_conn_.end()) return -1;
      hConn = it->second;
    }
    // GNS の Nagle はデフォルト 5ms。msquic / yojimbo 等は Nagle 相当の遅延を
    // 入れていないため、bench のフェアネスのため NoNagle を立てて即時送信する。
    int flags = reliable ? k_nSteamNetworkingSend_Reliable
                         : k_nSteamNetworkingSend_Unreliable;
    flags |= k_nSteamNetworkingSend_NoNagle;
    EResult r = iface_->SendMessageToConnection(
        hConn, data, static_cast<uint32>(len), flags, nullptr);
    // k_EResultIgnored: unreliable dropped due to NoDelay — not an error
    return (r == k_EResultOK || r == k_EResultIgnored) ? 0 : -1;
  }

  int recv(void* buf, size_t cap, size_t* out_len, uint32_t* out_conn_id) override {
    std::lock_guard<std::mutex> lock(mtx_);
    return inbox_.recv(buf, cap, out_len, out_conn_id);
  }

  void poll() override {
    // コールバックを同期的に発火させる (接続状態変化 / 到着通知)
    iface_->RunCallbacks();

    // 全接続からメッセージを受信する。scratch を再利用して tick ごとの
    // vector allocation を避ける。
    {
      std::lock_guard<std::mutex> lock(mtx_);
      poll_conns_.clear();
      poll_conns_.reserve(id_to_conn_.size());
      for (auto& [id, hConn] : id_to_conn_) poll_conns_.emplace_back(id, hConn);
    }

    for (auto& [id, hConn] : poll_conns_) {
      drain_connection(id, hConn);
    }
  }

  void close() override {
    if (listen_sock_ != k_HSteamListenSocket_Invalid) {
      {
        std::lock_guard<std::mutex> glock(g_gns.mtx);
        g_gns.listen_map.erase(listen_sock_);
      }
      iface_->CloseListenSocket(listen_sock_);
      listen_sock_ = k_HSteamListenSocket_Invalid;
    }

    std::vector<HSteamNetConnection> to_close;
    {
      std::lock_guard<std::mutex> lock(mtx_);
      for (auto& [id, hConn] : id_to_conn_) to_close.push_back(hConn);
      id_to_conn_.clear();
      conn_to_id_.clear();
      connected_ids_.clear();
      inbox_.clear();
    }
    for (auto hConn : to_close) {
      iface_->CloseConnection(hConn, 0, nullptr, false);
      std::lock_guard<std::mutex> glock(g_gns.mtx);
      g_gns.conn_map.erase(hConn);
    }
  }

  const char* name() const override { return "gns"; }
  bool supports(bool /*reliable*/) const override { return true; }
  size_t max_payload_bytes(bool /*reliable*/) const override { return 65536; }
  const char* flush_policy(bool /*reliable*/) const override { return "no_nagle"; }
  bool encryption_on() const override { return true; }

  // gns_status_callback から呼ばれる (g_gns.mtx は既に解放済み)
  void on_status_changed(SteamNetConnectionStatusChangedCallback_t* info) {
    auto hConn = info->m_hConn;
    auto state = info->m_info.m_eState;

    switch (state) {
      case k_ESteamNetworkingConnectionState_None:
        // 接続破棄時に発火するクリーンアップ通知 — 無視してよい
        break;

      case k_ESteamNetworkingConnectionState_Connecting: {
        // listen_socket 経由の新着接続かどうかを確認する。
        // クライアント側の接続は ConnectByIPAddress 直後に conn_to_id_ に登録済み
        // なので、ここでは見つかる → AcceptConnection を呼ばない。
        bool is_new = false;
        {
          std::lock_guard<std::mutex> lock(mtx_);
          if (conn_to_id_.find(hConn) == conn_to_id_.end()) {
            uint32_t id = next_id_++;
            conn_to_id_[hConn] = id;
            id_to_conn_[id] = hConn;
            is_new = true;
          }
        }
        if (is_new) {
          // mtx_ を解放した後で呼ぶことでデッドロックを回避
          iface_->AcceptConnection(hConn);
        }
        break;
      }

      case k_ESteamNetworkingConnectionState_Connected: {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = conn_to_id_.find(hConn);
        if (it != conn_to_id_.end()) {
          connected_ids_.insert(it->second);
        }
        break;
      }

      case k_ESteamNetworkingConnectionState_ClosedByPeer:
      case k_ESteamNetworkingConnectionState_ProblemDetectedLocally: {
        iface_->CloseConnection(hConn, 0, nullptr, false);
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = conn_to_id_.find(hConn);
        if (it != conn_to_id_.end()) {
          uint32_t id = it->second;
          connected_ids_.erase(id);
          id_to_conn_.erase(id);
          conn_to_id_.erase(it);
        }
        break;
      }

      default:
        break;
    }
  }

 private:
  void drain_connection(uint32_t id, HSteamNetConnection hConn) {
    for (;;) {
      SteamNetworkingMessage_t* msgs[GNS_RECV_BATCH];
      int n = iface_->ReceiveMessagesOnConnection(hConn, msgs, GNS_RECV_BATCH);
      if (n <= 0) return;
      {
        std::lock_guard<std::mutex> lock(mtx_);
        for (int i = 0; i < n; ++i) {
          const auto* p = static_cast<const uint8_t*>(msgs[i]->m_pData);
          inbox_.enqueue(id, p, msgs[i]->m_cbSize);
        }
      }
      for (int i = 0; i < n; ++i) msgs[i]->Release();
      if (n < GNS_RECV_BATCH) return;
    }
  }

  ISteamNetworkingSockets* iface_ = nullptr;
  HSteamListenSocket listen_sock_ = k_HSteamListenSocket_Invalid;

  std::mutex mtx_;
  std::unordered_map<uint32_t, HSteamNetConnection> id_to_conn_;
  std::unordered_map<HSteamNetConnection, uint32_t> conn_to_id_;
  std::unordered_set<uint32_t> connected_ids_;
  std::vector<std::pair<uint32_t, HSteamNetConnection>> poll_conns_;
  uint32_t next_id_ = 1;

  rudp_bench::ReusableInboundQueue inbox_;
};

// アダプタインスタンスへのルーティングを行うグローバルコールバック
static void gns_status_callback(SteamNetConnectionStatusChangedCallback_t* info) {
  GnsAdapter* adapter = nullptr;
  {
    std::lock_guard<std::mutex> glock(g_gns.mtx);
    auto state = info->m_info.m_eState;

    // listen_socket 経由の新着接続: サーバーアダプタを探してコネクションを登録
    if (state == k_ESteamNetworkingConnectionState_Connecting &&
        info->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid) {
      auto it = g_gns.listen_map.find(info->m_info.m_hListenSocket);
      if (it != g_gns.listen_map.end()) {
        adapter = it->second;
        g_gns.conn_map[info->m_hConn] = adapter;
      }
    } else {
      auto it = g_gns.conn_map.find(info->m_hConn);
      if (it != g_gns.conn_map.end()) {
        adapter = it->second;
        // 終端状態ではグローバルマップから削除
        if (state == k_ESteamNetworkingConnectionState_ClosedByPeer ||
            state == k_ESteamNetworkingConnectionState_ProblemDetectedLocally) {
          g_gns.conn_map.erase(it);
        }
      }
    }
  }
  if (adapter) adapter->on_status_changed(info);
}

}  // namespace

namespace rudp_bench {
void register_gns_adapter() {
  register_adapter("gns", []() { return std::make_unique<GnsAdapter>(); });
}
}  // namespace rudp_bench
