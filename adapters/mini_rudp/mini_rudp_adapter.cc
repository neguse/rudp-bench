#include "harness/adapter.h"
#include "harness/adapter_registry.h"
#include "harness/sliding_dedup_window.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <unordered_map>
#include <vector>

// ============================================================
// 設計メモ
// ============================================================
// 最小 reliable-UDP の参考実装。stop-and-go ではなく単純な per-seq ACK + 50ms
// タイムアウト再送。
//
// L11: クライアントは **1 本の UDP ソケットを全 conn で多重化**する（以前は
// conn 毎に別ソケット → 200conn で recvfrom/poll の syscall が膨大になり負荷を
// 出し切れず valid=0 だった）。多重化のためワイヤヘッダに論理 conn id (conv) を
// 持たせ、server は (src_addr, conv) で、client は conv だけで demux する。
//   ヘッダ(10B, packed): [u16 flags][u32 conv][u32 seq]
// ============================================================

namespace {

constexpr uint16_t FLAG_ACK = 1;
constexpr uint16_t FLAG_REL = 2;
constexpr size_t MAX_UDP_PAYLOAD = 65507;
constexpr size_t MAX_PENDING_RELIABLE_PER_CONN = 65'536;
constexpr auto RETX_TIMEOUT = std::chrono::milliseconds(50);
// L17: 256KB, uniform across the UDP adapters. A 2026-05-31 A/B confirmed a
// bigger buffer is no help (enet) or harmful (kcp bufferbloat), so 256KB stays.
constexpr int UDP_SOCKET_BUFFER_BYTES = 256 * 1024;

struct Header {
  uint16_t flags;
  uint32_t conv;  // L11: 論理 conn id（client が採番、wire で運ぶ）
  uint32_t seq;
} __attribute__((packed));

using clock_type = std::chrono::steady_clock;

struct PendingSend {
  std::vector<uint8_t> bytes;
  clock_type::time_point sent_at;
};

struct Conn {
  sockaddr_in peer{};
  uint32_t conv = 0;       // wire 上の論理 conn id
  uint32_t next_seq = 1;
  std::unordered_map<uint32_t, PendingSend> pending;  // 未 ACK (reliable)
  rudp_bench::SlidingDedupWindow received_seq;        // bounded duplicate suppress
};

struct PayloadView {
  const uint8_t* data = nullptr;
  size_t len = 0;
};

// server 側の (src_addr, conv) 複合キー。client 由来の conv は client 毎に 1..N
// で重複しうるので、送信元アドレスと組で初めて一意になる。
struct ConnKey {
  uint64_t addr;
  uint32_t conv;
  bool operator==(const ConnKey& o) const {
    return addr == o.addr && conv == o.conv;
  }
};
struct ConnKeyHash {
  size_t operator()(const ConnKey& k) const noexcept {
    size_t h = std::hash<uint64_t>{}(k.addr);
    h ^= std::hash<uint32_t>{}(k.conv) + 0x9e3779b9u + (h << 6) + (h >> 2);
    return h;
  }
};

uint64_t addr_key(const sockaddr_in& a) {
  return (static_cast<uint64_t>(a.sin_addr.s_addr) << 32) |
         static_cast<uint64_t>(a.sin_port);
}

void set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void tune_socket_buffers(int fd) {
  int bytes = UDP_SOCKET_BUFFER_BYTES;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes));
}

class MiniRudpAdapter : public rudp_bench::Adapter {
 public:
  ~MiniRudpAdapter() override { close(); }

  void server_listen(uint16_t port) override {
    server_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    int reuse = 1;
    ::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    tune_socket_buffers(server_fd_);  // L17
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    ::bind(server_fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    set_nonblock(server_fd_);
  }

  uint32_t client_connect(const char* host, uint16_t port) override {
    // L11: 1 本の共有ソケットを初回だけ生成。以降の conn は全てこの fd を多重化。
    if (client_fd_ < 0) {
      client_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
      tune_socket_buffers(client_fd_);  // L17
      set_nonblock(client_fd_);
    }
    uint32_t id = next_id_++;
    Conn c;
    c.conv = id;  // client の conv == adapter handle（conns_ のキー）
    c.peer.sin_family = AF_INET;
    c.peer.sin_port = htons(port);
    inet_pton(AF_INET, host, &c.peer.sin_addr);
    conns_[id] = std::move(c);
    return id;
  }

  bool is_connected(uint32_t) override { return true; }

  int send(uint32_t conn_id, const void* data, size_t len, bool reliable) override {
    if (len > max_payload_bytes(reliable)) return -1;
    auto* c = find_conn(conn_id);
    if (!c) return -1;
    if (reliable && c->pending.size() >= MAX_PENDING_RELIABLE_PER_CONN) {
      return -1;
    }
    Header h;
    h.flags = reliable ? FLAG_REL : 0;
    h.conv = c->conv;
    h.seq = c->next_seq++;

    if (reliable) {
      // L12: 送信バッファをプールから取り、未 ACK の間 pending が保持。ACK で
      // プールへ戻す（毎送信のヒープ alloc を排除）。
      std::vector<uint8_t> pkt = take_buffer();
      pkt.resize(sizeof(h) + len);
      std::memcpy(pkt.data(), &h, sizeof(h));
      std::memcpy(pkt.data() + sizeof(h), data, len);
      if (!raw_send(*c, pkt.data(), pkt.size())) {
        recycle_buffer(std::move(pkt));
        return -1;
      }
      auto now = clock_type::now();
      c->pending[h.seq] = PendingSend{std::move(pkt), now};
      schedule_retx(now);
      return 0;
    }

    send_scratch_.resize(sizeof(h) + len);
    std::memcpy(send_scratch_.data(), &h, sizeof(h));
    std::memcpy(send_scratch_.data() + sizeof(h), data, len);
    return raw_send(*c, send_scratch_.data(), send_scratch_.size()) ? 0 : -1;
  }

  // L13: 1 datagram ごとに ACK/重複などの control パケットが挟まると、上位の
  // harness drain ループは recv()==0 で止まり後続が遅延する。ここで内部ループし
  // 「配信可能ペイロードが取れる」か「ソケットが空」まで回し、control パケットは
  // 黙って処理して読み飛ばす。
  int recv(void* buf, size_t cap, size_t* out_len, uint32_t* out_conn_id) override {
    int fd = (server_fd_ >= 0) ? server_fd_ : client_fd_;
    if (fd < 0) return 0;
    for (;;) {
      sockaddr_in src{};
      socklen_t sl = sizeof(src);
      ssize_t n = ::recvfrom(fd, recv_scratch_, sizeof(recv_scratch_), 0,
                             reinterpret_cast<sockaddr*>(&src), &sl);
      if (n <= 0) return 0;  // EWOULDBLOCK / empty
      if (n < static_cast<ssize_t>(sizeof(Header))) continue;

      Header h;
      std::memcpy(&h, recv_scratch_, sizeof(h));

      Conn* c = nullptr;
      uint32_t out_id = 0;
      if (server_fd_ >= 0) {
        ConnKey key{addr_key(src), h.conv};
        auto it = id_by_key_.find(key);
        uint32_t id;
        if (it == id_by_key_.end()) {
          id = next_id_++;
          id_by_key_[key] = id;
          Conn nc;
          nc.conv = h.conv;
          nc.peer = src;
          conns_[id] = std::move(nc);
        } else {
          id = it->second;
        }
        c = &conns_[id];
        c->peer = src;  // path 変化に追従
        out_id = id;
      } else {
        auto it = conns_.find(h.conv);
        if (it == conns_.end()) continue;  // 未知 conv は捨てる
        c = &it->second;
        out_id = h.conv;
      }

      PayloadView pv;
      if (handle_packet(*c, h, recv_scratch_, static_cast<size_t>(n), &pv)) {
        if (pv.len > cap) continue;  // 入りきらない: 落として次へ
        if (pv.len > 0) std::memcpy(buf, pv.data, pv.len);
        *out_len = pv.len;
        *out_conn_id = out_id;
        return 1;
      }
      // control packet (ACK / 重複) → 読み飛ばして次の datagram へ
    }
  }

  void poll() override {
    if (conns_.empty()) return;
    auto now = clock_type::now();
    // L12: 何も期限到来していなければ全 conn × 全 pending の線形走査を丸ごと省略。
    if (now < earliest_retx_) return;

    auto next_earliest = clock_type::time_point::max();
    for (auto& [id, c] : conns_) {
      (void)id;
      for (auto& [seq, ps] : c.pending) {
        (void)seq;
        auto due = ps.sent_at + RETX_TIMEOUT;
        if (now >= due) {
          raw_send(c, ps.bytes.data(), ps.bytes.size());
          ps.sent_at = now;
          due = now + RETX_TIMEOUT;
        }
        if (due < next_earliest) next_earliest = due;
      }
    }
    earliest_retx_ = next_earliest;  // max() = 未 ACK 無し → 次回も即 early-out
  }

  void close() override {
    if (server_fd_ >= 0) { ::close(server_fd_); server_fd_ = -1; }
    if (client_fd_ >= 0) { ::close(client_fd_); client_fd_ = -1; }
    conns_.clear();
    id_by_key_.clear();
    free_buffers_.clear();
    earliest_retx_ = clock_type::time_point::max();
  }

  const char* name() const override { return "mini_rudp"; }
  bool supports(bool) const override { return true; }
  size_t max_payload_bytes(bool /*reliable*/) const override {
    return MAX_UDP_PAYLOAD - sizeof(Header);
  }
  const char* flush_policy(bool reliable) const override {
    return reliable ? "immediate_retransmit_poll" : "immediate";
  }
  bool encryption_on() const override { return false; }

 private:
  Conn* find_conn(uint32_t id) {
    auto it = conns_.find(id);
    return it == conns_.end() ? nullptr : &it->second;
  }

  // 共有 fd 経由で c.peer 宛てに送る（server/client 共通）。
  bool raw_send(const Conn& c, const void* data, size_t len) {
    int fd = (server_fd_ >= 0) ? server_fd_ : client_fd_;
    if (fd < 0) return false;
    ssize_t n = ::sendto(fd, data, len, 0,
                         reinterpret_cast<const sockaddr*>(&c.peer),
                         sizeof(c.peer));
    return n == static_cast<ssize_t>(len);
  }

  // 受信パケットを処理。配信可能ペイロードなら true。ACK/重複は false。
  bool handle_packet(Conn& c, const Header& h, const uint8_t* pkt, size_t n,
                     PayloadView* out) {
    if (h.flags & FLAG_ACK) {
      ack_pending(c, h.seq);
      return false;
    }
    if (h.flags & FLAG_REL) {
      Header ack{};
      ack.flags = FLAG_ACK;
      ack.conv = c.conv;
      ack.seq = h.seq;
      raw_send(c, &ack, sizeof(ack));
      if (!c.received_seq.insert(h.seq)) return false;  // 重複
    }
    out->data = pkt + sizeof(Header);
    out->len = n - sizeof(Header);
    return true;
  }

  void ack_pending(Conn& c, uint32_t seq) {
    auto it = c.pending.find(seq);
    if (it == c.pending.end()) return;
    recycle_buffer(std::move(it->second.bytes));
    c.pending.erase(it);
  }

  // L12: 送信バッファのフリーリスト（capacity を再利用）。
  std::vector<uint8_t> take_buffer() {
    if (free_buffers_.empty()) return std::vector<uint8_t>();
    std::vector<uint8_t> b = std::move(free_buffers_.back());
    free_buffers_.pop_back();
    b.clear();
    return b;
  }
  void recycle_buffer(std::vector<uint8_t>&& b) {
    b.clear();
    free_buffers_.push_back(std::move(b));
  }

  void schedule_retx(clock_type::time_point now) {
    auto due = now + RETX_TIMEOUT;
    if (due < earliest_retx_) earliest_retx_ = due;
  }

  int server_fd_ = -1;
  int client_fd_ = -1;
  std::unordered_map<uint32_t, Conn> conns_;
  std::unordered_map<ConnKey, uint32_t, ConnKeyHash> id_by_key_;
  std::vector<std::vector<uint8_t>> free_buffers_;
  std::vector<uint8_t> send_scratch_;
  uint8_t recv_scratch_[MAX_UDP_PAYLOAD];
  clock_type::time_point earliest_retx_ = clock_type::time_point::max();
  uint32_t next_id_ = 1;
};

}  // namespace

namespace rudp_bench {
void register_mini_rudp_adapter() {
  register_adapter("mini_rudp",
      []() { return std::make_unique<MiniRudpAdapter>(); });
}
}  // namespace rudp_bench
