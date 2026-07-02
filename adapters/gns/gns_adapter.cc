#include "harness/adapter.h"
#include "harness/adapter_registry.h"
#include "harness/inbound_queue.h"

#include <steam/steamnetworkingsockets.h>
#include <steam/isteamnetworkingutils.h>

// Internal GNS global (we link the static lib in-tree): UDP socket
// SO_SNDBUF/SO_RCVBUF, hardcoded to 256KB upstream with no public config.
// It also caps the SNP send loop at (size >> 11) packets per service-thread
// wakeup. 256KB is ~3.4ms of headroom at media_relay@50conns (75MB/s) and
// only 128 packets/think — both far too small under fanout load.
namespace SteamNetworkingSocketsLib {
extern int g_cbUDPSocketBufferSize;
}

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// Larger batch now that one ReceiveMessagesOnPollGroup call drains all conns.
constexpr int GNS_RECV_BATCH = 256;
constexpr size_t kDefaultGnsInboxLimit = 1u << 16;

size_t gns_inbox_limit() {
  const char* v = std::getenv("GNS_INBOX_MESSAGES");
  if (!v || !*v) return kDefaultGnsInboxLimit;
  errno = 0;
  char* end = nullptr;
  unsigned long long parsed = std::strtoull(v, &end, 10);
  if (end == v || *end != '\0' || errno == ERANGE || parsed == 0) {
    return kDefaultGnsInboxLimit;
  }
  if (parsed > std::numeric_limits<size_t>::max()) {
    return std::numeric_limits<size_t>::max();
  }
  return static_cast<size_t>(parsed);
}

struct GnsOptions {
  // Nagle ON by default: the canonical broadcast profiles are syscall-bound
  // (one sendmsg per packet, 64% of server CPU in the gprofng profile), and
  // 5ms coalescing turns ~10 small fanout messages into one MTU packet
  // (game_server c64: 151% -> 87% server CPU, media c50: 0.59 -> 0.93).
  bool use_nagle = true;
  bool split_lanes = false;
  bool encrypted = false;
  // Deviates from the L17 256KB convention: GNS sends ALL conns through one
  // socket on a single service thread, so at media_relay@50 (33k pkt/s out)
  // a 256KB SO_SNDBUF holds ~128 in-flight skbs vs a ~1100-packet BDP through
  // netem's 25ms delay — ENOBUFS drops as a buffer artifact, not protocol
  // loss. 4MB (= host rmem_max/wmem_max) clears it: c50 0.9389 -> 0.9584,
  // no regression on any other profile (2026-06-10 A/B, N=2).
  int socket_buffer_bytes = 4 * 1024 * 1024;
};

// GNS rate-limits ALL traffic (unreliable included) with a token bucket whose
// default ceiling is SendRateMax=256KB/s — that cap, not the wire, was the
// media_relay break at 50 conns (50 senders * 30Hz * 1000B = 1.5MB/s per conn,
// observed delivery 0.153 ~= 256KB/1.5MB). Raise the bucket to its config max
// and size the queues for fanout bursts instead of the 512KB default.
constexpr int32_t GNS_SEND_RATE_BPS = 0x10000000;       // 256MB/s (config max)
constexpr int32_t GNS_SEND_BUFFER_BYTES = 32 * 1024 * 1024;
constexpr int32_t GNS_RECV_BUFFER_BYTES = 32 * 1024 * 1024;
constexpr int32_t GNS_RECV_BUFFER_MESSAGES = 1 << 20;

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

void ensure_gns_init(int socket_buffer_bytes) {
  // Internal global, read when each UDP socket is created (after the adapter
  // constructor). Also feeds SNP's packets-per-think cap (size >> 11). One
  // bench process hosts one adapter instance, so last-write-wins is fine.
  SteamNetworkingSocketsLib::g_cbUDPSocketBufferSize = socket_buffer_bytes;
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
  explicit GnsAdapter(GnsOptions options = {}) : options_(options) {
    ensure_gns_init(options_.socket_buffer_bytes);
    iface_ = SteamNetworkingSockets();
    inbox_.set_limit(gns_inbox_limit());
    // One poll group for ALL connections so receive draining is a few
    // ReceiveMessagesOnPollGroup calls per tick instead of one
    // ReceiveMessagesOnConnection per conn. The per-conn path took the global
    // tables lock + each conn's m_pLock every tick, colliding head-on with the
    // single GNS service thread that holds m_pLock during decrypt — the cause
    // of the 1000conn collapse (0.993@600 -> 0.565@1000).
    poll_group_ = iface_->CreatePollGroup();
  }

  ~GnsAdapter() override { close(); }

  void server_listen(uint16_t port) override {
    SteamNetworkingIPAddr addr;
    addr.Clear();
    addr.m_port = port;

    auto opts = connection_opts();
    listen_sock_ = iface_->CreateListenSocketIP(
        addr, static_cast<int>(opts.size()), opts.data());
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

    auto opts = connection_opts();
    HSteamNetConnection hConn = iface_->ConnectByIPAddress(
        addr, static_cast<int>(opts.size()), opts.data());
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
    int flags = reliable ? k_nSteamNetworkingSend_Reliable
                         : k_nSteamNetworkingSend_Unreliable;
    if (!options_.use_nagle) flags |= k_nSteamNetworkingSend_NoNagle;
    if (options_.split_lanes) {
      SteamNetworkingMessage_t* msg =
          SteamNetworkingUtils()->AllocateMessage(static_cast<int>(len));
      if (!msg) return -1;
      std::memcpy(msg->m_pData, data, len);
      msg->m_cbSize = static_cast<int>(len);
      msg->m_conn = hConn;
      msg->m_nFlags = flags;
      msg->m_idxLane = reliable ? 0 : 1;
      SteamNetworkingMessage_t* msgs[1] = {msg};
      int64 result = 0;
      iface_->SendMessages(1, msgs, &result);
      return (result >= 0 || result == -k_EResultIgnored) ? 0 : -1;
    }
    EResult r = iface_->SendMessageToConnection(
        hConn, data, static_cast<uint32>(len), flags, nullptr);
    // k_EResultIgnored: unreliable dropped due to NoDelay — not an error
    return (r == k_EResultOK || r == k_EResultIgnored) ? 0 : -1;
  }

  // Broadcast fanout: one SendMessages call instead of count individual
  // sends. GNS groups the batch per connection, so the timestamp fetch,
  // handle lookup and the NoNagle "think" run once per conn per call instead
  // of once per message — the per-send overhead is the server bottleneck at
  // high fanout (e.g. game_server@96 ≈ 190k sends/s).
  size_t send_many(const uint32_t* conn_ids, size_t count, const void* data,
                   size_t len, bool reliable) override {
    if (count == 0) return 0;
    int flags = reliable ? k_nSteamNetworkingSend_Reliable
                         : k_nSteamNetworkingSend_Unreliable;
    if (!options_.use_nagle) flags |= k_nSteamNetworkingSend_NoNagle;

    scratch_conns_.clear();
    scratch_conns_.reserve(count);
    {
      std::lock_guard<std::mutex> lock(mtx_);
      for (size_t i = 0; i < count; ++i) {
        auto it = id_to_conn_.find(conn_ids[i]);
        scratch_conns_.push_back(it == id_to_conn_.end()
                                     ? k_HSteamNetConnection_Invalid
                                     : it->second);
      }
    }

    scratch_msgs_.clear();
    scratch_msgs_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      if (scratch_conns_[i] == k_HSteamNetConnection_Invalid) continue;
      SteamNetworkingMessage_t* msg =
          SteamNetworkingUtils()->AllocateMessage(static_cast<int>(len));
      if (!msg) break;
      std::memcpy(msg->m_pData, data, len);
      msg->m_cbSize = static_cast<int>(len);
      msg->m_conn = scratch_conns_[i];
      msg->m_nFlags = flags;
      if (options_.split_lanes) msg->m_idxLane = reliable ? 0 : 1;
      scratch_msgs_.push_back(msg);
    }
    if (scratch_msgs_.empty()) return 0;

    scratch_results_.assign(scratch_msgs_.size(), 0);
    iface_->SendMessages(static_cast<int>(scratch_msgs_.size()),
                         scratch_msgs_.data(), scratch_results_.data());
    size_t accepted = 0;
    for (int64 r : scratch_results_) {
      if (r >= 0 || r == -k_EResultIgnored) ++accepted;
    }
    return accepted;
  }

  int recv(void* buf, size_t cap, size_t* out_len, uint32_t* out_conn_id) override {
    std::lock_guard<std::mutex> lock(mtx_);
    return inbox_.recv(buf, cap, out_len, out_conn_id);
  }

  void poll() override {
    // コールバックを同期的に発火させる (接続状態変化 / 到着通知)
    iface_->RunCallbacks();

    // Drain ALL connections in one poll group instead of looping per-conn.
    // Each message carries its own m_conn, which we map back to our conn id.
    SteamNetworkingMessage_t* msgs[GNS_RECV_BATCH];
    for (;;) {
      int n = iface_->ReceiveMessagesOnPollGroup(poll_group_, msgs, GNS_RECV_BATCH);
      if (n <= 0) break;
      {
        std::lock_guard<std::mutex> lock(mtx_);
        for (int i = 0; i < n; ++i) {
          uint32_t id = 0;
          auto it = conn_to_id_.find(msgs[i]->m_conn);
          if (it != conn_to_id_.end()) id = it->second;
          inbox_.enqueue(id, static_cast<const uint8_t*>(msgs[i]->m_pData),
                         msgs[i]->m_cbSize);
        }
      }
      for (int i = 0; i < n; ++i) msgs[i]->Release();
      if (n < GNS_RECV_BATCH) break;
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
    uint64_t inbox_dropped = 0;
    {
      std::lock_guard<std::mutex> lock(mtx_);
      inbox_dropped = inbox_.dropped();
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
    if (poll_group_ != k_HSteamNetPollGroup_Invalid) {
      iface_->DestroyPollGroup(poll_group_);
      poll_group_ = k_HSteamNetPollGroup_Invalid;
    }
    if (inbox_dropped > 0) {
      std::fprintf(stderr, "gns_inbox_dropped: %llu\n",
                   (unsigned long long)inbox_dropped);
      std::fflush(stderr);
    }
  }

  const char* name() const override { return "gns"; }
  bool supports(bool /*reliable*/) const override { return true; }
  size_t max_payload_bytes(bool /*reliable*/) const override { return 65536; }
  const char* flush_policy(bool /*reliable*/) const override {
    if (options_.split_lanes) {
      return options_.use_nagle ? "nagle_split_lanes" : "no_nagle_split_lanes";
    }
    return options_.use_nagle ? "nagle" : "no_nagle";
  }
  bool encryption_on() const override { return options_.encrypted; }
  // TFRC + token bucket。adapter が SendRateMax を 256MB/s に引き上げて
  // レート制御を実質無効化している（audit §10）。
  const char* congestion_control() const override { return "tfrc_maxrate_boosted"; }
  const char* thread_model() const override { return "internal_worker"; }

  // connected_ids_ の推移から conn_peak / 切断数を報告する (以前は未オーバー
  // ライドで常に {0,0,0} だった)。callback スレッドから更新されるため atomic。
  rudp_bench::ConnectionStats connection_stats() const override {
    return {connected_peak_.load(), shutdown_by_transport_.load(),
            shutdown_by_peer_.load()};
  }

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
        // Route this conn's inbound messages through the shared poll group
        // (both server-accepted and client-initiated conns reach Connected).
        if (options_.split_lanes) {
          EResult r = iface_->ConfigureConnectionLanes(hConn, 2, nullptr, nullptr);
          if (r != k_EResultOK) std::abort();
        }
        iface_->SetConnectionPollGroup(hConn, poll_group_);
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = conn_to_id_.find(hConn);
        if (it != conn_to_id_.end()) {
          if (connected_ids_.insert(it->second).second) {
            uint32_t now =
                static_cast<uint32_t>(connected_ids_.size());
            uint32_t peak = connected_peak_.load();
            while (now > peak &&
                   !connected_peak_.compare_exchange_weak(peak, now)) {
            }
          }
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
          if (connected_ids_.erase(id) > 0) {
            // connected 状態から落ちた接続だけ切断としてカウントする
            if (state == k_ESteamNetworkingConnectionState_ClosedByPeer) {
              shutdown_by_peer_.fetch_add(1, std::memory_order_relaxed);
            } else {
              shutdown_by_transport_.fetch_add(1, std::memory_order_relaxed);
            }
          }
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
  // Shared config for listen sockets and outbound connections. Unencrypted=3
  // ("required") must be set on BOTH peers; every harness process goes through
  // this same path, so the handshake agrees.
  std::vector<SteamNetworkingConfigValue_t> connection_opts() {
    std::vector<SteamNetworkingConfigValue_t> opts;
    opts.emplace_back();
    opts.back().SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                       reinterpret_cast<void*>(gns_status_callback));
    opts.emplace_back();
    opts.back().SetInt32(k_ESteamNetworkingConfig_SendRateMin, GNS_SEND_RATE_BPS);
    opts.emplace_back();
    opts.back().SetInt32(k_ESteamNetworkingConfig_SendRateMax, GNS_SEND_RATE_BPS);
    opts.emplace_back();
    opts.back().SetInt32(k_ESteamNetworkingConfig_SendBufferSize,
                         GNS_SEND_BUFFER_BYTES);
    opts.emplace_back();
    opts.back().SetInt32(k_ESteamNetworkingConfig_RecvBufferSize,
                         GNS_RECV_BUFFER_BYTES);
    opts.emplace_back();
    opts.back().SetInt32(k_ESteamNetworkingConfig_RecvBufferMessages,
                         GNS_RECV_BUFFER_MESSAGES);
    if (!options_.encrypted) {
      opts.emplace_back();
      opts.back().SetInt32(k_ESteamNetworkingConfig_Unencrypted, 3);
    }
    return opts;
  }

  ISteamNetworkingSockets* iface_ = nullptr;
  HSteamListenSocket listen_sock_ = k_HSteamListenSocket_Invalid;
  HSteamNetPollGroup poll_group_ = k_HSteamNetPollGroup_Invalid;
  GnsOptions options_;

  std::mutex mtx_;
  std::unordered_map<uint32_t, HSteamNetConnection> id_to_conn_;
  std::unordered_map<HSteamNetConnection, uint32_t> conn_to_id_;
  std::unordered_set<uint32_t> connected_ids_;
  uint32_t next_id_ = 1;

  // 接続イベントの累計 (connection_stats() で読み取る)。更新は mtx_ 保持下の
  // callback から行うが、読み取りが const メソッドなので atomic にしている。
  std::atomic<uint32_t> connected_peak_{0};
  std::atomic<uint32_t> shutdown_by_transport_{0};
  std::atomic<uint32_t> shutdown_by_peer_{0};

  rudp_bench::ReusableInboundQueue inbox_;

  // send_many scratch buffers (harness calls send_many from one thread).
  std::vector<HSteamNetConnection> scratch_conns_;
  std::vector<SteamNetworkingMessage_t*> scratch_msgs_;
  std::vector<int64> scratch_results_;
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
  register_adapter("gns_encrypted", []() {
    GnsOptions o;
    o.encrypted = true;
    return std::make_unique<GnsAdapter>(o);
  });
  register_adapter("gns_no_nagle", []() {
    GnsOptions o;
    o.use_nagle = false;
    return std::make_unique<GnsAdapter>(o);
  });
  register_adapter("gns_smallbuf", []() {
    GnsOptions o;
    o.socket_buffer_bytes = 256 * 1024;
    return std::make_unique<GnsAdapter>(o);
  });
  register_adapter("gns_split_lanes", []() {
    GnsOptions o;
    o.split_lanes = true;
    return std::make_unique<GnsAdapter>(o);
  });
}
}  // namespace rudp_bench
