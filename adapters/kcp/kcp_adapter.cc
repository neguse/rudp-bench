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
// KcpAdapter – skeleton (stubs)
// ============================================================
class KcpAdapter : public rudp_bench::Adapter {
 public:
  ~KcpAdapter() override { close(); }

  void server_listen(uint16_t /*port*/) override {}
  uint32_t client_connect(const char* /*host*/, uint16_t /*port*/) override { return 0; }
  bool is_connected(uint32_t /*conn_id*/) override { return false; }
  int send(uint32_t /*conn_id*/, const void* /*data*/, size_t /*len*/, bool /*reliable*/) override { return -1; }
  int recv(void* /*buf*/, size_t /*cap*/, size_t* /*out_len*/, uint32_t* /*out_conn_id*/) override { return 0; }
  void poll() override {}
  void close() override {}

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
};

}  // namespace

namespace rudp_bench {
void register_kcp_adapter() {
  register_adapter("kcp", []() { return std::make_unique<KcpAdapter>(); });
}
}  // namespace rudp_bench
