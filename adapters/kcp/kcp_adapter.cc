#include "harness/adapter.h"
#include "harness/adapter_registry.h"

extern "C" {
#include "ikcp.h"
}

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
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
static constexpr int KCP_SND_WND = 128;
static constexpr int KCP_RCV_WND = 128;
static constexpr int KCP_MAX_FRAME = KCP_MTU + 32;
static constexpr size_t RECV_BUF_SIZE = 65536 + 8;

inline IUINT32 now_ms() {
  using namespace std::chrono;
  return static_cast<IUINT32>(
      duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
          .count() & 0xFFFFFFFFu);
}

static void set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static uint64_t addr_key(const sockaddr_in& a) {
  return (static_cast<uint64_t>(a.sin_addr.s_addr) << 32) |
         static_cast<uint64_t>(a.sin_port);
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
  uint32_t     conn_id = 0;
  bool         connected = false;

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

struct InboundMsg {
  uint32_t conn_id;
  std::vector<uint8_t> data;
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
    set_nonblock(server_fd_);
  }

  uint32_t client_connect(const char* /*host*/, uint16_t /*port*/) override { return 0; }
  bool is_connected(uint32_t /*conn_id*/) override { return false; }
  int send(uint32_t /*conn_id*/, const void* /*data*/, size_t /*len*/, bool /*reliable*/) override { return -1; }

  int recv(void* buf, size_t cap, size_t* out_len, uint32_t* out_conn_id) override {
    if (inbox_.empty()) return 0;
    auto& m = inbox_.front();
    if (m.data.size() > cap) {
      *out_len = m.data.size();
      *out_conn_id = m.conn_id;
      inbox_.pop_front();
      return -1;
    }
    std::memcpy(buf, m.data.data(), m.data.size());
    *out_len = m.data.size();
    *out_conn_id = m.conn_id;
    inbox_.pop_front();
    return 1;
  }

  void poll() override {
    drain_socket();
    IUINT32 now = now_ms();
    for (auto& [id, conn] : conns_) {
      if (conn->kcp) {
        ikcp_update(conn->kcp, now);
        drain_kcp(conn.get());
      }
    }
  }

  void close() override {
    conns_.clear();
    id_by_addrconv_.clear();
    if (server_fd_ >= 0) { ::close(server_fd_); server_fd_ = -1; }
    if (client_fd_ >= 0) { ::close(client_fd_); client_fd_ = -1; }
  }

  const char* name() const override { return "kcp"; }
  bool supports(bool) const override { return true; }
  bool encryption_on() const override { return false; }

 private:
  int server_fd_ = -1;
  int client_fd_ = -1;
  bool is_server_ = false;
  std::unordered_map<uint32_t, std::unique_ptr<KcpConn>> conns_;
  std::unordered_map<AddrConvKey, uint32_t, AddrConvKeyHash> id_by_addrconv_;
  uint32_t next_id_ = 1;
  std::deque<InboundMsg> inbox_;

  // KCP インスタンスを持つか確認、なければ生成 (server 側の遅延生成)
  void ensure_kcp(KcpConn* conn) {
    if (conn->kcp) return;
    conn->kcp = ikcp_create(conn->conv, &conn->out_ctx);
    ikcp_setoutput(conn->kcp, kcp_output_cb);
    ikcp_nodelay(conn->kcp, 1, 10, 2, 1);
    ikcp_setmtu(conn->kcp, KCP_MTU);
    ikcp_wndsize(conn->kcp, KCP_SND_WND, KCP_RCV_WND);
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

  // socket から受信した生バイト列を KCP に投入 or unreliable inbox に積む
  void drain_socket() {
    static uint8_t raw[RECV_BUF_SIZE];

    if (is_server_) {
      sockaddr_in src{};
      socklen_t sl = sizeof(src);
      ssize_t n;
      while ((n = ::recvfrom(server_fd_, raw, sizeof(raw), 0,
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
          ensure_kcp(conn.get());
          ikcp_input(conn->kcp, reinterpret_cast<char*>(raw + 1),
                     static_cast<long>(n - 1));
        } else if (prefix == PREFIX_RAW && n >= 5) {
          IUINT32 conv; std::memcpy(&conv, raw + 1, 4);
          AddrConvKey key{addr_key(src), conv};
          auto it = id_by_addrconv_.find(key);
          uint32_t id = (it == id_by_addrconv_.end())
                        ? alloc_server_conn(src, conv)
                        : it->second;
          enqueue_raw(id, raw + 5, static_cast<size_t>(n - 5));
        }
      }
    }
    // client path: added in next step
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
    InboundMsg m;
    m.conn_id = conn_id;
    m.data.assign(data, data + len);
    inbox_.push_back(std::move(m));
  }
};

}  // namespace

namespace rudp_bench {
void register_kcp_adapter() {
  register_adapter("kcp", []() { return std::make_unique<KcpAdapter>(); });
}
}  // namespace rudp_bench
