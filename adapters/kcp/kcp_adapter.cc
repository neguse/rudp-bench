#include "harness/adapter.h"
#include "harness/adapter_registry.h"
#include "harness/inbound_queue.h"

extern "C" {
#include "ikcp.h"
}

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <unordered_map>
#include <vector>

// ============================================================
// 設計メモ
// ============================================================
// KCP は ARQ プロトコルのみ提供し、UDP ソケットを含まない。
// このアダプタは raw_udp と同様に自前で UDP ソケットを管理する。
//
// ワイヤフォーマット (先頭 1 バイトで種別を区別):
//   0x01 + KCP フレーム          … reliable (KCP ARQ 経由)
//   0x00 + conv(4B LE) + ペイロード … unreliable (KCP バイパス、raw sendto)
//
// KCP "conv" (会話 ID) = クライアント側 conn_id と同値。
// server 側は受信パケットの conv で接続を識別し、独自の conn_id を払い出す。
// ============================================================

namespace {

static constexpr uint8_t PREFIX_KCP = 0x01;
static constexpr uint8_t PREFIX_RAW = 0x00;
static constexpr int KCP_MTU     = 1400;
static constexpr int KCP_MAX_FRAME = KCP_MTU + 32;
static constexpr size_t RECV_BUF_SIZE = 65536 + 8;

// kcp tuning. Defaults are the config chosen by the 2026-05-31 tuning sweep
// (see docs/measurements/2026-05-31-kcp-tuning). Env overrides exist only to
// re-run the sweep; production uses the defaults.
//
// Sweep findings (mixed 50/50, 1% loss, server=1 phys core, N=3):
//   - interval is the 600<->1000 lever: 5ms lifts 1000 (HoL stall recovery) but
//     costs 600 (extra flush overhead when not stalled); 10ms is the reverse.
//     5ms wins net for the high-conn tail, which is where kcp competes.
//   - window: at 5ms, 256 beats 384/512/1024 at BOTH 600 and 1000 (the 512 from
//     the first pass was a regression). Bigger window does not help under the
//     single-thread ARQ HoL ceiling; it just adds buffering latency.
//   - REJECTED (measured worse): drain-interleave (helps 600 but craters 1000 to
//     ~0.50 and saturates CPU), phase-desync (neutral), fastresend=1 (more
//     spurious retx), minrto>30 (no gain). All left as off-by-default toggles.
//   KCP_WND        snd/rcv window in packets               (default 256)
//   KCP_INTERVAL   flush/retx granularity ms               (default 5)
//   KCP_RESEND     fastresend dup-ack threshold            (default 2)
//   KCP_NC         nocwnd (1=disable congestion window)    (default 1)
//   KCP_MINRTO     min RTO ms override (0=nodelay default 30)
//   KCP_DEAD_LINK  xmit count before ikcp marks state=-1   (default stock)
//   KCP_DRAIN_CHUNK  re-drain socket every N conns during scan (0=off, rejected)
//   KCP_DESYNC     stagger per-conn flush phase (0/1, rejected as neutral)
struct KcpTuning {
  int wnd, interval, resend, nc, minrto, drain_chunk, desync;
};
inline const KcpTuning& kcp_tuning() {
  static const KcpTuning t = []() {
    auto gi = [](const char* k, int d) {
      const char* v = std::getenv(k);
      return (v && *v) ? std::atoi(v) : d;
    };
    return KcpTuning{gi("KCP_WND", 256), gi("KCP_INTERVAL", 5),
                     gi("KCP_RESEND", 2), gi("KCP_NC", 1), gi("KCP_MINRTO", 0),
                     gi("KCP_DRAIN_CHUNK", 0), gi("KCP_DESYNC", 0)};
  }();
  return t;
}

inline IUINT32 kcp_dead_link_override() {
  const char* v = std::getenv("KCP_DEAD_LINK");
  if (!v || !*v) return 0;
  int parsed = std::atoi(v);
  return parsed > 0 ? static_cast<IUINT32>(parsed) : 0;
}

inline IUINT32 now_ms() {
  using namespace std::chrono;
  return static_cast<IUINT32>(
      duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
          .count() & 0xFFFFFFFFu);
}

// Apply nodelay then force the sub-10ms interval past ikcp_nodelay's floor
// (stock ikcp clamps interval<10 to 10; interval is a public ikcpcb field so we
// set it directly WITHOUT modifying the vendored submodule). Optionally stagger
// the flush phase per conn (KCP_DESYNC) so not all conns become due in the same
// poll — that spreads the update burst and lets socket draining interleave.
static inline void kcp_apply_tuning(ikcpcb* kcp, IUINT32 conv) {
  const auto& t = kcp_tuning();
  ikcp_nodelay(kcp, 1, t.interval, t.resend, t.nc);
  kcp->interval = static_cast<IUINT32>(t.interval);
  if (t.minrto > 0) kcp->rx_minrto = static_cast<IUINT32>(t.minrto);
  ikcp_setmtu(kcp, KCP_MTU);
  ikcp_wndsize(kcp, t.wnd, t.wnd);
  if (IUINT32 dead_link = kcp_dead_link_override(); dead_link > 0) {
    kcp->dead_link = dead_link;
  }
  if (t.desync && t.interval > 1) {
    kcp->updated = 1;  // skip ikcp_update's "first call" ts_flush reset
    kcp->ts_flush = now_ms() + (conv % static_cast<IUINT32>(t.interval));
  }
}

static void set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// L17: 256KB SO_RCVBUF/SO_SNDBUF, uniform across UDP adapters. NOTE: a bigger
// buffer HURTS kcp — a 2026-05-31 A/B at 1000conn showed 1MB bufferbloats the
// ARQ (more queued segments -> spurious RTO retransmits), dropping 0.78 -> 0.52.
// 256KB is best for kcp. KCP_RCVBUF_KB overrides (sweeps only).
static void tune_socket_buffers(int fd) {
  int kb = 256;
  if (const char* v = std::getenv("KCP_RCVBUF_KB"); v && *v) {
    int e = std::atoi(v);
    if (e > 0) kb = e;
  }
  int bytes = kb * 1024;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes));
}

static uint64_t addr_key(const sockaddr_in& a) {
  return (static_cast<uint64_t>(a.sin_addr.s_addr) << 32) |
         static_cast<uint64_t>(a.sin_port);
}

static bool time_due(IUINT32 now, IUINT32 deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

// KCP output callback コンテキスト (ikcp_create の user ポインタに渡す)
struct KcpOutputCtx {
  int fd = -1;
  bool is_server = false;
  sockaddr_in peer_addr{};
};

// KCP が送信フレームを生成したとき呼ばれるコールバック。
// PREFIX_KCP を先頭に付与して UDP で送出する。
static int kcp_output_cb(const char* buf, int len, ikcpcb* /*kcp*/,
                          void* user) {
  if (len <= 0 || len > KCP_MAX_FRAME) return -1;
  char tmp[KCP_MAX_FRAME + 1];
  tmp[0] = static_cast<char>(PREFIX_KCP);
  std::memcpy(tmp + 1, buf, static_cast<size_t>(len));
  const auto* ctx = static_cast<const KcpOutputCtx*>(user);
  if (ctx->is_server) {
    ::sendto(ctx->fd, tmp, static_cast<size_t>(len + 1), 0,
             reinterpret_cast<const sockaddr*>(&ctx->peer_addr),
             sizeof(ctx->peer_addr));
  } else {
    ::send(ctx->fd, tmp, static_cast<size_t>(len + 1), 0);
  }
  return 0;
}

struct KcpConn {
  ikcpcb*      kcp = nullptr;
  KcpOutputCtx out_ctx;
  IUINT32      conv = 0;
  IUINT32      next_update = 0;
  uint32_t     conn_id = 0;
  bool         connected = false;
  bool         update_due = true;

  KcpConn() = default;
  ~KcpConn() { if (kcp) { ikcp_release(kcp); kcp = nullptr; } }
  KcpConn(const KcpConn&) = delete;
  KcpConn& operator=(const KcpConn&) = delete;
};

// (addr_key, conv) 複合キー – server 側コネクション識別に使用
struct AddrConvKey {
  uint64_t addr_k;
  IUINT32  conv;
  bool operator==(const AddrConvKey& o) const {
    return addr_k == o.addr_k && conv == o.conv;
  }
};
struct AddrConvKeyHash {
  size_t operator()(const AddrConvKey& k) const noexcept {
    size_t h = std::hash<uint64_t>{}(k.addr_k);
    h ^= std::hash<uint32_t>{}(k.conv) + 0x9e3779b9u + (h << 6) + (h >> 2);
    return h;
  }
};

// ============================================================
// KcpAdapter – server-side receive + connection tracking
// ============================================================
class KcpAdapter : public rudp_bench::Adapter {
 public:
  ~KcpAdapter() override { close(); }

  // ----------------------------------------------------------
  // server_listen: UDP ソケットをバインドして非ブロッキング化
  // ----------------------------------------------------------
  void server_listen(uint16_t port) override {
    is_server_ = true;
    server_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (server_fd_ < 0) std::abort();
    int reuse = 1;
    ::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0)
      std::abort();
    tune_socket_buffers(server_fd_);  // L17
    set_nonblock(server_fd_);
  }

  // ----------------------------------------------------------
  // client_connect: 共有ソケットを生成 (初回のみ)、KCP インスタンスを作成。
  // 全コネクションが同一サーバ宛てであることを前提に 1 ソケットを共有する。
  // conv = conn_id とし、server 側が受信フレームから接続を特定できるようにする。
  // ----------------------------------------------------------
  uint32_t client_connect(const char* host, uint16_t port) override {
    if (client_fd_ < 0) {
      client_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
      if (client_fd_ < 0) std::abort();
      tune_socket_buffers(client_fd_);  // L17
      set_nonblock(client_fd_);
      sockaddr_in srv{};
      srv.sin_family = AF_INET;
      srv.sin_port   = htons(port);
      inet_pton(AF_INET, host, &srv.sin_addr);
      ::connect(client_fd_, reinterpret_cast<sockaddr*>(&srv), sizeof(srv));
    }
    uint32_t id = next_id_++;
    auto conn = std::make_unique<KcpConn>();
    conn->conn_id         = id;
    conn->conv            = static_cast<IUINT32>(id);
    conn->out_ctx.fd      = client_fd_;
    conn->out_ctx.is_server = false;
    conn->kcp = ikcp_create(conn->conv, &conn->out_ctx);
    ikcp_setoutput(conn->kcp, kcp_output_cb);
    kcp_apply_tuning(conn->kcp, conn->conv);
    // KCP はハンドシェイク不要 – ソケット生成後即座に connected
    conn->connected = true;
    conns_[id] = std::move(conn);
    force_scan_ = true;  // L8: ensure the next poll() services the new conn
    return id;
  }

  bool is_connected(uint32_t conn_id) override {
    auto it = conns_.find(conn_id);
    if (it == conns_.end()) return false;
    KcpConn* conn = it->second.get();
    if (conn->connected && conn->kcp && conn->kcp->state == static_cast<IUINT32>(-1)) {
      mark_dead(conn);
    }
    return conn->connected;
  }

  // ----------------------------------------------------------
  // send: reliable → KCP ARQ 経由 / unreliable → raw sendto bypass
  //
  // unreliable ワイヤ: [PREFIX_RAW][conv:4LE][payload]
  // conv を含めることで server/client 双方が conn_id を特定できる。
  // ----------------------------------------------------------
  int send(uint32_t conn_id, const void* data, size_t len, bool reliable) override {
    if (len > max_payload_bytes(reliable)) return -1;
    auto it = conns_.find(conn_id);
    if (it == conns_.end()) return -1;
    KcpConn* conn = it->second.get();
    if (!conn->connected) return -1;

    if (reliable) {
      ensure_kcp(conn);
      if (ikcp_send(conn->kcp, static_cast<const char*>(data),
                    static_cast<int>(len)) < 0) {
        return -1;
      }
      conn->update_due = true;
      force_scan_ = true;  // L8: flush the freshly-queued segment next poll
      return 0;
    }

    // unreliable bypass: raw UDP with conv header
    raw_send_scratch_.resize(5 + len);
    raw_send_scratch_[0] = PREFIX_RAW;
    std::memcpy(raw_send_scratch_.data() + 1, &conn->conv, 4);
    std::memcpy(raw_send_scratch_.data() + 5, data, len);
    ssize_t n;
    if (is_server_) {
      n = ::sendto(server_fd_, raw_send_scratch_.data(), raw_send_scratch_.size(), 0,
                   reinterpret_cast<const sockaddr*>(&conn->out_ctx.peer_addr),
                   sizeof(conn->out_ctx.peer_addr));
    } else {
      n = ::send(client_fd_, raw_send_scratch_.data(), raw_send_scratch_.size(), 0);
    }
    return (n == static_cast<ssize_t>(raw_send_scratch_.size())) ? 0 : -1;
  }

  int recv(void* buf, size_t cap, size_t* out_len, uint32_t* out_conn_id) override {
    return inbox_.recv(buf, cap, out_len, out_conn_id);
  }

  void poll() override {
    bool had_input = drain_socket();
    IUINT32 now = now_ms();
    // L8: skip the O(conns) scan when nothing is due and no KCP input arrived.
    // The harness spins poll() far faster than the 5ms KCP interval, so between
    // due boundaries this is the common case and becomes O(1). At a due boundary
    // we scan all conns, but since every conn shares one interval the due-set
    // ~= all conns — a heap would not beat the linear scan there.
    if (!force_scan_ && !had_input && !time_due(now, earliest_due_)) return;
    force_scan_ = false;

    const int drain_chunk = kcp_tuning().drain_chunk;
    IUINT32 next_earliest = now + 60'000;  // far-future sentinel
    bool any = false;
    int since_drain = 0;
    for (auto& [id, conn] : conns_) {
      (void)id;
      if (!conn->connected || !conn->kcp) continue;
      // Interleave socket draining so a long 1000-conn update scan does not
      // starve the receive side (input piling in the socket buffer -> overflow
      // -> KCP retransmit -> window stall). Refresh `now` after re-draining so
      // later conns see their true due time.
      if (drain_chunk > 0 && ++since_drain >= drain_chunk) {
        drain_socket();
        now = now_ms();
        since_drain = 0;
      }
      if (conn->update_due || conn->next_update == 0 ||
          time_due(now, conn->next_update)) {
        ikcp_update(conn->kcp, now);
        if (conn->kcp->state == static_cast<IUINT32>(-1)) {
          mark_dead(conn.get());
          continue;
        }
        drain_kcp(conn.get());
        conn->next_update = ikcp_check(conn->kcp, now);
        conn->update_due = false;
      }
      // Track the soonest next_update to decide the next early-out.
      if (!any || time_due(next_earliest, conn->next_update)) {
        next_earliest = conn->next_update;
        any = true;
      }
    }
    earliest_due_ = any ? next_earliest : (now + 60'000);
  }

  void close() override {
    conns_.clear();
    id_by_addrconv_.clear();
    if (server_fd_ >= 0) { ::close(server_fd_); server_fd_ = -1; }
    if (client_fd_ >= 0) { ::close(client_fd_); client_fd_ = -1; }
    shutdown_by_transport_ = 0;
  }

  const char* name() const override { return "kcp"; }
  bool supports(bool) const override { return true; }
  size_t max_payload_bytes(bool reliable) const override {
    return reliable ? 65536 : 65502;
  }
  const char* flush_policy(bool reliable) const override {
    return reliable ? "poll_update" : "immediate";
  }
  bool encryption_on() const override { return false; }
  rudp_bench::ConnectionStats connection_stats() const override {
    rudp_bench::ConnectionStats s;
    s.shutdown_by_transport = shutdown_by_transport_;
    return s;
  }

 private:
  int server_fd_ = -1;
  int client_fd_ = -1;
  bool is_server_ = false;
  std::unordered_map<uint32_t, std::unique_ptr<KcpConn>> conns_;
  std::unordered_map<AddrConvKey, uint32_t, AddrConvKeyHash> id_by_addrconv_;
  uint32_t next_id_ = 1;
  // L8: incremental earliest-due tracking so poll() can early-out in O(1) when
  // nothing is due. force_scan_ is set whenever a conn newly needs an update
  // outside poll() (new conn, reliable send, KCP input) so we never skip it.
  IUINT32 earliest_due_ = 0;
  bool force_scan_ = true;
  std::vector<uint8_t> raw_send_scratch_;
  std::array<uint8_t, RECV_BUF_SIZE> recv_scratch_{};
  rudp_bench::ReusableInboundQueue inbox_;
  uint32_t shutdown_by_transport_ = 0;

  // KCP インスタンスを持つか確認、なければ生成 (server 側の遅延生成)
  void ensure_kcp(KcpConn* conn) {
    if (!conn || !conn->connected || conn->kcp) return;
    conn->kcp = ikcp_create(conn->conv, &conn->out_ctx);
    ikcp_setoutput(conn->kcp, kcp_output_cb);
    kcp_apply_tuning(conn->kcp, conn->conv);
    conn->update_due = true;
  }

  // server 側: 新規 (addr, conv) の接続エントリを払い出す
  uint32_t alloc_server_conn(const sockaddr_in& src, IUINT32 conv) {
    uint32_t id = next_id_++;
    id_by_addrconv_[AddrConvKey{addr_key(src), conv}] = id;
    auto conn = std::make_unique<KcpConn>();
    conn->conn_id = id;
    conn->conv    = conv;
    conn->out_ctx.fd        = server_fd_;
    conn->out_ctx.is_server = true;
    conn->out_ctx.peer_addr = src;
    conn->connected = true;
    conns_[id] = std::move(conn);
    return id;
  }

  // socket から受信した生バイト列を KCP に投入 or unreliable inbox に積む。
  // KCP フレームを 1 つでも input したら true（poll() がスキャン要否を判断、L8）。
  bool drain_socket() {
    uint8_t* raw = recv_scratch_.data();
    const size_t raw_cap = recv_scratch_.size();
    bool had_kcp_input = false;

    if (is_server_) {
      sockaddr_in src{};
      socklen_t sl = sizeof(src);
      ssize_t n;
      while ((n = ::recvfrom(server_fd_, raw, raw_cap, 0,
                             reinterpret_cast<sockaddr*>(&src), &sl)) > 0) {
        if (n < 1) continue;
        uint8_t prefix = raw[0];
        if (prefix == PREFIX_KCP && n > 5) {
          IUINT32 conv = ikcp_getconv(raw + 1);
          AddrConvKey key{addr_key(src), conv};
          auto it = id_by_addrconv_.find(key);
          uint32_t id = (it == id_by_addrconv_.end())
                        ? alloc_server_conn(src, conv)
                        : it->second;
          auto& conn = conns_[id];
          if (!conn->connected) continue;
          ensure_kcp(conn.get());
          if (!conn->kcp) continue;
          ikcp_input(conn->kcp, reinterpret_cast<char*>(raw + 1),
                     static_cast<long>(n - 1));
          conn->update_due = true;
          had_kcp_input = true;
        } else if (prefix == PREFIX_RAW && n >= 5) {
          IUINT32 conv; std::memcpy(&conv, raw + 1, 4);
          AddrConvKey key{addr_key(src), conv};
          auto it = id_by_addrconv_.find(key);
          uint32_t id = (it == id_by_addrconv_.end())
                        ? alloc_server_conn(src, conv)
                        : it->second;
          if (!conns_[id]->connected) continue;
          enqueue_raw(id, raw + 5, static_cast<size_t>(n - 5));
        }
      }
    } else {
      // client: 共有ソケットから受信し conv で KCP インスタンスに振り分け
      ssize_t n;
      while ((n = ::recv(client_fd_, raw, raw_cap, 0)) > 0) {
        if (n < 1) continue;
        uint8_t prefix = raw[0];
        if (prefix == PREFIX_KCP && n > 5) {
          IUINT32 conv = ikcp_getconv(raw + 1);
          auto it = conns_.find(static_cast<uint32_t>(conv));
          if (it != conns_.end() && it->second->connected && it->second->kcp) {
            ikcp_input(it->second->kcp, reinterpret_cast<char*>(raw + 1),
                       static_cast<long>(n - 1));
            it->second->update_due = true;
            had_kcp_input = true;
          }
        } else if (prefix == PREFIX_RAW && n >= 5) {
          IUINT32 conv; std::memcpy(&conv, raw + 1, 4);
          auto it = conns_.find(static_cast<uint32_t>(conv));
          if (it != conns_.end() && it->second->connected) {
            enqueue_raw(it->second->conn_id, raw + 5, static_cast<size_t>(n - 5));
          }
        }
      }
    }
    return had_kcp_input;
  }

  // KCP デコード済みメッセージを inbox_ に移す
  void drain_kcp(KcpConn* conn) {
    char buf[65536];
    int n;
    while ((n = ikcp_recv(conn->kcp, buf, static_cast<int>(sizeof(buf)))) > 0) {
      enqueue_raw(conn->conn_id, reinterpret_cast<const uint8_t*>(buf),
                  static_cast<size_t>(n));
    }
  }

  void enqueue_raw(uint32_t conn_id, const uint8_t* data, size_t len) {
    inbox_.enqueue(conn_id, data, len);
  }

  void mark_dead(KcpConn* conn) {
    if (!conn || !conn->connected) return;
    conn->connected = false;
    conn->update_due = false;
    ++shutdown_by_transport_;
  }
};

}  // namespace

namespace rudp_bench {
void register_kcp_adapter() {
  register_adapter("kcp", []() { return std::make_unique<KcpAdapter>(); });
}
}  // namespace rudp_bench
