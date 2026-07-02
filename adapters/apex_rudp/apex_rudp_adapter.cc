#include "harness/adapter.h"
#include "harness/adapter_registry.h"
#include "harness/inbound_queue.h"
#include "harness/socket_buffer.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

// ============================================================
// apex_rudp
// ============================================================
// Benchmark-oriented reliable UDP:
// - one main UDP data socket per process, multiplexed by logical conv id
// - server can split client ACK-only traffic onto a small control socket
// - reliable messages are unordered and SACKed with a 64-bit receive bitmap
// - ACKs piggyback on data packets; standalone ACKs are delayed briefly
// - server unreliable echoes use a dedicated TX worker to keep receive hot path
//   below saturation at the 1000-connection mixed workload
// - retransmit scans are skipped until the earliest pending packet is due
//
// This intentionally targets the harness workload: many independent small
// messages where ordered reliable delivery would only add head-of-line stalls.
//
// 輻輳制御・ペーシングは意図的に実装していない (ベンチ設計):
// バックプレッシャは reliable pending 上限 (MAX_PENDING_RELIABLE_PER_CONN) と
// TX キュー上限 (MAX_TX_QUEUE) のみで、輻輳への適応は測定対象ワークロード
// (閉ループ echo) の性質に任せている。一般網向けの実装ではない点に注意。
// ============================================================

namespace {

using clock_type = std::chrono::steady_clock;

constexpr uint8_t FLAG_REL = 0x01;
constexpr uint8_t FLAG_ACK_ONLY = 0x02;
constexpr uint8_t FLAG_HAS_ACK = 0x04;
constexpr uint8_t FLAG_BATCH = 0x08;

constexpr size_t MAX_UDP_PAYLOAD = 65507;
constexpr size_t HEADER_BYTES = 1 + 4 + 4 + 4 + 8;  // flags, conv, seq, ack, ack_bits
constexpr size_t BATCH_FRAME_LEN_BYTES = 2;
constexpr size_t MAX_PAYLOAD = MAX_UDP_PAYLOAD - HEADER_BYTES;
constexpr size_t MAX_PENDING_RELIABLE_PER_CONN = 4096;
// in-flight 窓: 受信側 SACK bitmap が 64bit 固定なので、未ACK最小 seq から
// SEND_WINDOW 個以内しかワイヤに出さない (span 最大 63 → 受信側 delta < 64 が
// 常に成り立ち、RecvSackWindow が未配送パケットを取りこぼさない)。
// pending (最大 MAX_PENDING_RELIABLE_PER_CONN) はキューとして維持し、送出だけ
// この窓でゲートする。
constexpr uint32_t SEND_WINDOW = 64;
constexpr size_t INBOX_LIMIT = 1u << 20;
constexpr int DEFAULT_SOCKET_BUFFER_KB = 4096;
constexpr uint32_t DEFAULT_MAX_CONNECTIONS = 4096;
// RTO: SRTT/RTTVAR (RFC 6298 相当) による適応値。INITIAL_RTO は RTT サンプルが
// 取れるまでの初期値。
constexpr auto INITIAL_RTO = std::chrono::milliseconds(100);
constexpr auto MIN_RTO = std::chrono::milliseconds(10);
constexpr auto MAX_RTO = std::chrono::milliseconds(2000);
constexpr auto MAX_RETX_BACKOFF = std::chrono::milliseconds(3000);
constexpr auto RTO_GRANULARITY = std::chrono::milliseconds(1);
// 送出失敗 (TX バックプレッシャ) 時の再試行間隔
constexpr auto TX_RETRY_DELAY = std::chrono::milliseconds(1);
// SACK の穴が何回連続で観測されたら fast retransmit するか
constexpr uint8_t FAST_RETX_SKIPS = 3;
constexpr int DEFAULT_ACK_DELAY_MS = 2;
constexpr size_t RECV_BATCH = 64;
constexpr size_t TX_BATCH = 256;
constexpr size_t MAX_DRAIN_PACKETS = 4096;
constexpr size_t MAX_TX_QUEUE = 1u << 20;
constexpr size_t MAX_RX_QUEUE = 1u << 20;
constexpr size_t DEFAULT_CLIENT_RECV_BUDGET = 64;
constexpr int DEFAULT_RELIABLE_TIMEOUT_MS = 10'000;

struct PacketHeader {
  uint8_t flags = 0;
  uint32_t conv = 0;
  uint32_t seq = 0;
  uint32_t ack = 0;
  uint64_t ack_bits = 0;
};

uint32_t load_u32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

uint16_t load_u16(const uint8_t* p) {
  uint16_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

uint64_t load_u64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

void store_u16(uint8_t* p, uint16_t v) {
  std::memcpy(p, &v, sizeof(v));
}

void store_u32(uint8_t* p, uint32_t v) {
  std::memcpy(p, &v, sizeof(v));
}

void store_u64(uint8_t* p, uint64_t v) {
  std::memcpy(p, &v, sizeof(v));
}

PacketHeader parse_header(const uint8_t* p) {
  PacketHeader h;
  h.flags = p[0];
  h.conv = load_u32(p + 1);
  h.seq = load_u32(p + 5);
  h.ack = load_u32(p + 9);
  h.ack_bits = load_u64(p + 13);
  return h;
}

void write_header(uint8_t* p, const PacketHeader& h) {
  p[0] = h.flags;
  store_u32(p + 1, h.conv);
  store_u32(p + 5, h.seq);
  store_u32(p + 9, h.ack);
  store_u64(p + 13, h.ack_bits);
}

uint64_t addr_key(const sockaddr_in& a) {
  return (static_cast<uint64_t>(a.sin_addr.s_addr) << 32) |
         static_cast<uint64_t>(a.sin_port);
}

bool same_endpoint(const sockaddr_in& a, const sockaddr_in& b) {
  return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

bool seq32_after(uint32_t a, uint32_t b) {
  return static_cast<int32_t>(a - b) > 0;
}

void set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void tune_socket_buffers(int fd, const char* socket_role) {
  int kb = DEFAULT_SOCKET_BUFFER_KB;
  if (const char* v = std::getenv("APEX_RCVBUF_KB"); v && *v) {
    int parsed = std::atoi(v);
    if (parsed > 0) kb = parsed;
  }
  int bytes = kb * 1024;
  rudp_bench::tune_udp_socket_buffers(fd, bytes, bytes, "apex_rudp",
                                      socket_role);
}

bool async_send_enabled_for(bool is_server) {
  static const int mode = []() {
    const char* v = std::getenv("APEX_ASYNC_SEND");
    if (!v || !*v || std::strcmp(v, "0") == 0) return 0;
    if (std::strcmp(v, "server") == 0) return 1;
    if (std::strcmp(v, "client") == 0) return 2;
    return 3;
  }();
  return mode == 3 || (is_server && mode == 1) || (!is_server && mode == 2);
}

bool async_unreliable_server_enabled() {
  static const bool enabled = []() {
    const char* v = std::getenv("APEX_ASYNC_UNRELIABLE_SERVER");
    if (!v || !*v) return true;
    return v && std::atoi(v) != 0;
  }();
  return enabled;
}

bool rx_worker_enabled_for(bool is_server) {
  static const bool enabled = []() {
    const char* v = std::getenv("APEX_RX_WORKER");
    return v && std::atoi(v) != 0;
  }();
  return is_server && enabled;
}

size_t tx_worker_count_for(bool is_server) {
  static const size_t configured = []() {
    const char* v = std::getenv("APEX_TX_WORKERS");
    if (!v || !*v) return size_t{1};
    int parsed = std::atoi(v);
    if (parsed <= 0) return size_t{1};
    return static_cast<size_t>(std::min(parsed, 8));
  }();
  return is_server ? configured : size_t{1};
}

size_t client_shard_count_for(uint32_t hinted_connections) {
  static const int configured = []() {
    const char* v = std::getenv("APEX_CLIENT_SHARDS");
    if (!v || !*v) return 1;
    if (std::strcmp(v, "conn") == 0 || std::strcmp(v, "per_conn") == 0 ||
        std::strcmp(v, "auto") == 0) {
      return 0;
    }
    int parsed = std::atoi(v);
    return parsed > 0 ? parsed : 1;
  }();
  if (configured == 0) return std::max<uint32_t>(1, hinted_connections);
  return static_cast<size_t>(configured);
}

size_t recv_empty_drain_budget() {
  static const size_t budget = []() {
    const char* enabled = std::getenv("APEX_RECV_DRAIN_ON_EMPTY");
    if (enabled && std::strcmp(enabled, "0") == 0) return size_t{0};
    const char* v = std::getenv("APEX_RECV_EMPTY_DRAINS");
    if (!v || !*v) return size_t{1} << 20;
    int parsed = std::atoi(v);
    if (parsed <= 0) return size_t{0};
    return static_cast<size_t>(parsed);
  }();
  return budget;
}

bool recv_drain_on_empty_enabled() {
  static const bool enabled = []() {
    const char* v = std::getenv("APEX_RECV_DRAIN_ON_EMPTY");
    return !v || std::atoi(v) != 0;
  }();
  return enabled;
}

size_t client_recv_budget_per_poll() {
  static const size_t budget = []() {
    const char* v = std::getenv("APEX_CLIENT_RECV_BUDGET");
    if (!v || !*v) return DEFAULT_CLIENT_RECV_BUDGET;
    int parsed = std::atoi(v);
    if (parsed <= 0) return std::numeric_limits<size_t>::max();
    return static_cast<size_t>(parsed);
  }();
  return budget;
}

bool split_ack_enabled() {
  static const bool enabled = []() {
    const char* v = std::getenv("APEX_SPLIT_ACK");
    if (!v || !*v) return true;
    return v && std::atoi(v) != 0;
  }();
  return enabled;
}

std::chrono::milliseconds ack_delay() {
  static const auto delay = []() {
    const char* v = std::getenv("APEX_ACK_DELAY_MS");
    if (!v || !*v) return std::chrono::milliseconds(DEFAULT_ACK_DELAY_MS);
    int parsed = std::atoi(v);
    if (parsed < 0) parsed = 0;
    return std::chrono::milliseconds(parsed);
  }();
  return delay;
}

// adapter インスタンス生成時に一度だけ読む (getenv をホットパスから外す)。
// static キャッシュではなくインスタンス単位なのは、テストが env を差し替えて
// adapter を作り直せるようにするため。
std::chrono::milliseconds reliable_timeout_from_env() {
  const char* v = std::getenv("APEX_RELIABLE_TIMEOUT_MS");
  if (!v || !*v) return std::chrono::milliseconds(DEFAULT_RELIABLE_TIMEOUT_MS);
  int parsed = std::atoi(v);
  if (parsed <= 0) return std::chrono::milliseconds(DEFAULT_RELIABLE_TIMEOUT_MS);
  return std::chrono::milliseconds(parsed);
}

// 接続数上限の明示指定 (0 = 未指定。hint ベースの既定値を使う)
uint32_t conn_limit_from_env() {
  const char* v = std::getenv("APEX_MAX_CONNECTIONS");
  if (!v || !*v) return 0;
  long parsed = std::atol(v);
  if (parsed <= 0) return 0;
  return static_cast<uint32_t>(std::min(parsed, 1'000'000L));
}

class RecvSackWindow {
 public:
  bool insert(uint32_t seq) {
    if (seq == 0) return false;
    if (highest_ == 0) {
      highest_ = seq;
      bits_ = 1;
      return true;
    }
    if (seq32_after(seq, highest_)) {
      uint32_t shift = seq - highest_;
      bits_ = (shift >= 64) ? 1 : ((bits_ << shift) | 1);
      highest_ = seq;
      return true;
    }
    uint32_t delta = highest_ - seq;
    if (delta >= 64) return false;
    uint64_t mask = uint64_t{1} << delta;
    if (bits_ & mask) return false;
    bits_ |= mask;
    return true;
  }

  bool have_ack() const { return highest_ != 0; }
  uint32_t ack() const { return highest_; }
  uint64_t ack_bits() const { return bits_; }

 private:
  uint32_t highest_ = 0;
  uint64_t bits_ = 0;
};

struct PendingSend {
  uint32_t seq = 0;
  std::vector<uint8_t> bytes;
  clock_type::time_point first_sent_at{};  // 初回送出時刻 (transmitted 時のみ有効)
  clock_type::time_point sent_at{};        // 最終送出時刻 (transmitted 時のみ有効)
  bool transmitted = false;  // 一度でもワイヤに出したか (in-flight 窓ゲート)
  uint8_t retx_count = 0;    // 再送回数 (バックオフ指数 / Karn の除外判定)
  uint8_t skip_acks = 0;     // SACK の穴として観測された回数 (fast retransmit)
};

struct Conn {
  sockaddr_in peer{};
  int fd = -1;
  uint32_t conv = 0;
  uint32_t next_seq = 1;
  bool active = false;
  bool ack_dirty = false;
  bool in_ack_list = false;   // ack_list_ (dirty リスト) に載っているか
  bool in_retx_list = false;  // retx_list_ (dirty リスト) に載っているか
  bool rtt_valid = false;     // srtt/rttvar が初期化済みか
  clock_type::time_point ack_due = clock_type::time_point::max();
  clock_type::duration srtt{};
  clock_type::duration rttvar{};
  clock_type::duration rto = INITIAL_RTO;
  RecvSackWindow recv_window;
  std::deque<PendingSend> pending;
};

struct Peer {
  // conv はリモートが任意指定できる 32bit 値なので vector index にしない
  // (旧実装は resize(conv + 1) で conv=0xFFFFFFFF が ~16GB 確保になっていた)
  std::unordered_map<uint32_t, uint32_t> id_by_conv;
};

struct TxDatagram {
  sockaddr_in peer{};
  int fd = -1;
  std::vector<uint8_t> owned;
  const uint8_t* external = nullptr;
  size_t len = 0;

  const uint8_t* data() const {
    return external ? external : owned.data();
  }

  size_t size() const {
    return external ? len : owned.size();
  }
};

struct RxDatagram {
  sockaddr_in src{};
  std::vector<uint8_t> bytes;
};

struct FanoutJob {
  std::vector<uint8_t> bytes;
  std::vector<sockaddr_in> peers;
  size_t stride = 0;
};

bool ack_covers(uint32_t ack, uint64_t ack_bits, uint32_t seq) {
  if (ack == 0 || seq == 0 || seq32_after(seq, ack)) return false;
  uint32_t delta = ack - seq;
  if (delta >= 64) return false;
  return (ack_bits & (uint64_t{1} << delta)) != 0;
}

class ApexRudpAdapter : public rudp_bench::Adapter {
 public:
  ApexRudpAdapter() {
    inbox_.set_limit(INBOX_LIMIT);
    direct_batch_.reserve(TX_BATCH);
    fanout_conns_.reserve(TX_BATCH);
    fanout_headers_.reserve(TX_BATCH);
  }
  ~ApexRudpAdapter() override { close(); }

  void hint_connections(uint32_t n) override {
    hinted_connections_ = n;
    conns_.reserve(n);
    peers_.reserve(8);
    fanout_conns_.reserve(n);
    fanout_headers_.reserve(n);
  }

  void server_listen(uint16_t port) override {
    is_server_ = true;
    server_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (server_fd_ < 0) std::abort();
    int reuse = 1;
    ::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    tune_socket_buffers(server_fd_, "server");
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
      std::abort();
    }
    set_nonblock(server_fd_);
    if (split_ack_enabled()) {
      server_ack_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
      if (server_ack_fd_ < 0) std::abort();
      ::setsockopt(server_ack_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
      tune_socket_buffers(server_ack_fd_, "server_ack");
      sockaddr_in ack_addr = a;
      ack_addr.sin_port = htons(static_cast<uint16_t>(port + 1));
      if (::bind(server_ack_fd_, reinterpret_cast<sockaddr*>(&ack_addr),
                 sizeof(ack_addr)) != 0) {
        std::abort();
      }
      set_nonblock(server_ack_fd_);
    }
    if (rx_worker_enabled_for(true)) start_rx_worker();
    if (async_send_enabled_for(true) || async_unreliable_server_enabled()) {
      start_tx_worker();
    }
  }

  uint32_t client_connect(const char* host, uint16_t port) override {
    is_server_ = false;
    if (client_fds_.empty()) {
      sockaddr_in srv{};
      srv.sin_family = AF_INET;
      srv.sin_port = htons(port);
      if (inet_pton(AF_INET, host, &srv.sin_addr) != 1) std::abort();
      server_peer_ = srv;
      server_ack_peer_ = srv;
      server_ack_peer_.sin_port = htons(static_cast<uint16_t>(port + 1));

      size_t shards = client_shard_count_for(hinted_connections_);
      if (hinted_connections_ > 0) {
        shards = std::min<size_t>(shards, hinted_connections_);
      }
      shards = std::max<size_t>(1, shards);
      client_fds_.reserve(shards);
      for (size_t i = 0; i < shards; ++i) {
        int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) std::abort();
        tune_socket_buffers(fd, "client");
        set_nonblock(fd);
        client_fds_.push_back(fd);
      }
      client_fd_ = client_fds_.front();
      if (async_send_enabled_for(false)) start_tx_worker();
    }

    uint32_t id = next_id_++;
    Conn c;
    c.active = true;
    c.conv = id;
    c.fd = client_fds_[(id - 1) % client_fds_.size()];
    c.peer = server_peer_;
    conns_.push_back(std::move(c));
    ++active_conns_;
    connected_peak_ = std::max<uint32_t>(connected_peak_, active_conns_);
    return id;
  }

  bool is_connected(uint32_t conn_id) override { return find_conn(conn_id) != nullptr; }

  int send(uint32_t conn_id, const void* data, size_t len, bool reliable) override {
    if (len > max_payload_bytes(reliable)) return -1;
    Conn* c = find_conn(conn_id);
    if (!c) return -1;
    if (reliable && c->pending.size() >= MAX_PENDING_RELIABLE_PER_CONN) return -1;

    uint32_t seq = reliable ? next_packet_seq(*c) : 0;
    std::vector<uint8_t> pkt = take_buffer();
    pkt.resize(HEADER_BYTES + len);
    PacketHeader h;
    h.flags = reliable ? FLAG_REL : 0;
    h.conv = c->conv;
    h.seq = seq;
    attach_ack(*c, &h, reliable);
    write_header(pkt.data(), h);
    if (len > 0) {
      std::memcpy(pkt.data() + HEADER_BYTES, data, len);
    }

    if (reliable) {
      auto now = clock_type::now();
      // in-flight 窓ゲート: 窓外 (未ACK最小 seq から SEND_WINDOW 以上先) は
      // キューに積むだけで送出しない。ACK で窓が進んだ時 (pump_pending) か
      // service_retransmits が初回送出する。
      const bool in_window =
          c->pending.empty() || (seq - c->pending.front().seq) < SEND_WINDOW;
      PendingSend p;
      p.seq = seq;
      p.bytes = std::move(pkt);
      c->pending.push_back(std::move(p));
      PendingSend& pending = c->pending.back();
      if (in_window) {
        if (!queue_send_ref(*c, pending.bytes.data(), pending.bytes.size())) {
          recycle_buffer(std::move(pending.bytes));
          c->pending.pop_back();
          return -1;
        }
        pending.transmitted = true;
        pending.first_sent_at = now;
        pending.sent_at = now;
        note_ack_sent(*c, h);
        schedule_retx(now + c->rto);
      }
      add_to_retx_list(*c);
    } else {
      bool queued = (is_server_ && async_unreliable_server_enabled())
                        ? queue_send_owned_async(*c, std::move(pkt))
                        : queue_send_owned(*c, std::move(pkt));
      if (!queued) {
        recycle_buffer(std::move(pkt));
        return -1;
      }
      note_ack_sent(*c, h);
    }
    return 0;
  }

  size_t send_many(const uint32_t* conn_ids, size_t count,
                   const void* data, size_t len, bool reliable) override {
    if (len > max_payload_bytes(reliable)) return 0;
    if (reliable || !is_server_ || !async_unreliable_server_enabled()) {
      return Adapter::send_many(conn_ids, count, data, len, reliable);
    }

    fanout_conns_.clear();
    fanout_headers_.clear();
    // FanoutJob の作業バッファは tx worker から回収したものを再利用する
    FanoutJob job = take_fanout_job();
    job.stride = HEADER_BYTES + len;
    job.peers.reserve(count);
    job.bytes.reserve(job.stride * count);

    for (size_t i = 0; i < count; ++i) {
      Conn* c = find_conn(conn_ids[i]);
      if (!c) continue;

      size_t offset = job.bytes.size();
      job.bytes.resize(offset + job.stride);
      uint8_t* pkt = job.bytes.data() + offset;

      PacketHeader h;
      h.conv = c->conv;
      attach_ack(*c, &h, false);
      write_header(pkt, h);
      if (len > 0) {
        std::memcpy(pkt + HEADER_BYTES, data, len);
      }

      job.peers.push_back(c->peer);
      fanout_conns_.push_back(c);
      fanout_headers_.push_back(h);
    }

    size_t accepted = queue_fanout_job_async(std::move(job));
    for (size_t i = 0; i < accepted; ++i) {
      note_ack_sent(*fanout_conns_[i], fanout_headers_[i]);
    }
    return accepted;
  }

  int recv(void* buf, size_t cap, size_t* out_len, uint32_t* out_conn_id) override {
    // client 側の予算ロジック:
    // - batch 受信を観測した client は 1 poll あたり client_recv_budget_left_
    //   件までしか返さない (server の batch 到着で recv ばかり回って送信側が
    //   飢えるのを防ぐ)。予算は poll() 先頭で補充される。
    // - inbox が空のときは poll を待たずに socket を直接 drain して遅延を削る。
    //   ただし recv_empty_drains_left_ 回まで (poll() 先頭で補充)。
    // 注意: cap 不足のメッセージは inbox_ 側 (-1) でサイレント破棄される。
    if (!is_server_ && batch_recv_seen_ && client_recv_budget_left_ == 0) return 0;
    if (!is_server_ && recv_drain_on_empty_enabled() && inbox_.empty() &&
        recv_empty_drains_left_ > 0) {
      flush_direct_batch();
      drain_socket(MAX_DRAIN_PACKETS / 4);
      --recv_empty_drains_left_;
    }
    int r = inbox_.recv(buf, cap, out_len, out_conn_id);
    if (r == 1 && !is_server_ && batch_recv_seen_ &&
        client_recv_budget_left_ != std::numeric_limits<size_t>::max()) {
      --client_recv_budget_left_;
    }
    return r;
  }

  void poll() override {
    if (!is_server_) client_recv_budget_left_ = client_recv_budget_per_poll();
    flush_direct_batch();
    drain_socket(MAX_DRAIN_PACKETS);
    recv_empty_drains_left_ = recv_empty_drain_budget();
    auto now = clock_type::now();
    if (now >= earliest_retx_) {
      service_retransmits(now);
    }
    if (now >= earliest_ack_) {
      service_standalone_acks(now);
    }
  }

  void close() override {
    flush_direct_batch();
    stop_rx_worker();
    stop_tx_worker();
    if (server_fd_ >= 0) {
      ::close(server_fd_);
      server_fd_ = -1;
    }
    for (int fd : client_fds_) {
      if (fd >= 0) ::close(fd);
    }
    client_fds_.clear();
    client_fd_ = -1;
    if (server_ack_fd_ >= 0) {
      ::close(server_ack_fd_);
      server_ack_fd_ = -1;
    }
    conns_.clear();
    peers_.clear();
    ack_list_.clear();
    retx_list_.clear();
    active_conns_ = 0;
    free_buffers_.clear();
    {
      std::lock_guard<std::mutex> lock(recycled_mu_);
      recycled_buffers_.clear();
      fanout_pool_.clear();
    }
    inbox_.clear();
    batch_recv_seen_ = false;
    next_id_ = 1;
    connected_peak_ = 0;
    shutdown_by_transport_ = 0;
    hinted_connections_ = 0;
    earliest_ack_ = clock_type::time_point::max();
    earliest_retx_ = clock_type::time_point::max();
    client_recv_budget_left_ = std::numeric_limits<size_t>::max();
  }

  const char* name() const override { return "apex_rudp"; }
  bool supports(bool) const override { return true; }
  size_t max_payload_bytes(bool /*reliable*/) const override { return MAX_PAYLOAD; }
  // 接続数上限: APEX_MAX_CONNECTIONS > hint ベース (+余裕) > 既定値の順で決まる。
  // server_conn_for() はこの値を超える新規 Conn 生成を破棄する。
  uint32_t max_connections() const override {
    if (conn_limit_env_ > 0) return conn_limit_env_;
    if (hinted_connections_ > 0) {
      return hinted_connections_ + hinted_connections_ / 8 + 64;
    }
    return DEFAULT_MAX_CONNECTIONS;
  }
  const char* flush_policy(bool reliable) const override {
    return reliable ? "piggyback_sack_retransmit"
                    : "server_async_unreliable_piggyback_ack";
  }
  bool encryption_on() const override { return false; }
  // 輻輳制御は意図的に非実装（ファイル冒頭コメント参照）。
  const char* congestion_control() const override { return "none"; }
  const char* thread_model() const override { return "adapter_worker"; }
  rudp_bench::ConnectionStats connection_stats() const override {
    rudp_bench::ConnectionStats s;
    s.connected_peak = connected_peak_;
    s.shutdown_by_transport = shutdown_by_transport_;
    return s;
  }

 private:
  Conn* find_conn(uint32_t id) {
    if (id == 0 || id > conns_.size()) return nullptr;
    Conn& c = conns_[id - 1];
    return c.active ? &c : nullptr;
  }

  int send_fd_for(const Conn& c) const {
    if (is_server_) return server_fd_;
    return c.fd >= 0 ? c.fd : client_fd_;
  }

  int send_fd_for(const TxDatagram& d) const {
    if (d.fd >= 0) return d.fd;
    return is_server_ ? server_fd_ : client_fd_;
  }

  uint32_t next_packet_seq(Conn& c) {
    uint32_t seq = c.next_seq++;
    if (seq == 0) seq = c.next_seq++;
    return seq;
  }

  bool client_sharding_enabled() const {
    return !is_server_ && client_fds_.size() > 1;
  }

  bool send_one(int fd, const sockaddr_in& peer, const uint8_t* data, size_t len) {
    if (fd < 0) return false;
    while (true) {
      ssize_t n = ::sendto(fd, data, len, MSG_DONTWAIT,
                           reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
      if (n == static_cast<ssize_t>(len)) return true;
      if (n < 0 && errno == EINTR) continue;
      return false;
    }
  }

  Conn* server_conn_for(const sockaddr_in& src, uint32_t conv, bool create) {
    uint64_t key = addr_key(src);
    auto peer_it = peers_.find(key);
    if (peer_it != peers_.end()) {
      Peer& peer = peer_it->second;
      auto it = peer.id_by_conv.find(conv);
      if (it != peer.id_by_conv.end() && it->second != 0) {
        Conn* existing_conn = find_conn(it->second);
        if (existing_conn) return existing_conn;
        if (!create) return nullptr;
      }
    }
    if (!create) return nullptr;
    // 任意 src からの無制限 Conn 生成を防ぐ: 上限超過のパケットは破棄
    if (active_conns_ >= max_connections()) return nullptr;

    Peer& peer = peers_[key];
    uint32_t id = next_id_++;
    peer.id_by_conv[conv] = id;
    Conn c;
    c.active = true;
    c.conv = conv;
    c.fd = server_fd_;
    c.peer = src;
    conns_.push_back(std::move(c));
    ++active_conns_;
    connected_peak_ = std::max<uint32_t>(connected_peak_, active_conns_);
    return &conns_.back();
  }

  bool queue_send_ref(const Conn& c, const uint8_t* data, size_t len) {
    int fd = send_fd_for(c);
    if (fd < 0) return false;
    if (!async_send_enabled_for(is_server_)) {
      if (client_sharding_enabled()) {
        return send_one(fd, c.peer, data, len);
      }
      if (direct_batch_.size() >= MAX_TX_QUEUE) return false;
      TxDatagram d;
      d.peer = c.peer;
      d.fd = fd;
      d.external = data;
      d.len = len;
      direct_batch_.push_back(std::move(d));
      if (direct_batch_.size() >= TX_BATCH) flush_direct_batch();
      return true;
    }

    TxDatagram d;
    d.peer = c.peer;
    d.fd = fd;
    d.owned.resize(len);
    std::memcpy(d.owned.data(), data, len);
    {
      std::lock_guard<std::mutex> lock(tx_mu_);
      if (tx_pending_datagrams_ >= MAX_TX_QUEUE) return false;
      tx_queue_.push_back(std::move(d));
      ++tx_pending_datagrams_;
    }
    tx_cv_.notify_one();
    return true;
  }

  bool queue_send_owned(const Conn& c, std::vector<uint8_t>&& bytes) {
    int fd = send_fd_for(c);
    if (fd < 0) return false;
    if (!async_send_enabled_for(is_server_)) {
      if (client_sharding_enabled()) {
        bool sent = send_one(fd, c.peer, bytes.data(), bytes.size());
        if (sent) recycle_buffer(std::move(bytes));
        return sent;
      }
      if (direct_batch_.size() >= MAX_TX_QUEUE) return false;
      TxDatagram d;
      d.peer = c.peer;
      d.fd = fd;
      d.owned = std::move(bytes);
      direct_batch_.push_back(std::move(d));
      if (direct_batch_.size() >= TX_BATCH) flush_direct_batch();
      return true;
    }

    TxDatagram d;
    d.peer = c.peer;
    d.fd = fd;
    d.owned = std::move(bytes);
    {
      std::lock_guard<std::mutex> lock(tx_mu_);
      if (tx_pending_datagrams_ >= MAX_TX_QUEUE) return false;
      tx_queue_.push_back(std::move(d));
      ++tx_pending_datagrams_;
    }
    tx_cv_.notify_one();
    return true;
  }

  bool queue_send_owned_async(const Conn& c, std::vector<uint8_t>&& bytes) {
    int fd = send_fd_for(c);
    if (fd < 0) return false;
    {
      std::lock_guard<std::mutex> lock(tx_mu_);
      if (tx_pending_datagrams_ >= MAX_TX_QUEUE) return false;
      TxDatagram d;
      d.peer = c.peer;
      d.fd = fd;
      d.owned = std::move(bytes);
      tx_queue_.push_back(std::move(d));
      ++tx_pending_datagrams_;
    }
    tx_cv_.notify_one();
    return true;
  }

  size_t queue_send_owned_async_batch(std::vector<TxDatagram>& batch) {
    if ((is_server_ ? server_fd_ : client_fd_) < 0 || batch.empty()) return 0;
    size_t accepted = 0;
    {
      std::lock_guard<std::mutex> lock(tx_mu_);
      size_t room = tx_pending_datagrams_ >= MAX_TX_QUEUE
                        ? 0
                        : MAX_TX_QUEUE - tx_pending_datagrams_;
      accepted = std::min(batch.size(), room);
      for (size_t i = 0; i < accepted; ++i) {
        tx_queue_.push_back(std::move(batch[i]));
      }
      tx_pending_datagrams_ += accepted;
    }
    if (accepted > 0) {
      if (tx_workers_.size() > 1) {
        tx_cv_.notify_all();
      } else {
        tx_cv_.notify_one();
      }
    }
    return accepted;
  }

  size_t queue_fanout_job_async(FanoutJob&& job) {
    if ((is_server_ ? server_fd_ : client_fd_) < 0 || job.peers.empty()) return 0;
    size_t accepted = 0;
    {
      std::lock_guard<std::mutex> lock(tx_mu_);
      size_t room = tx_pending_datagrams_ >= MAX_TX_QUEUE
                        ? 0
                        : MAX_TX_QUEUE - tx_pending_datagrams_;
      accepted = std::min(job.peers.size(), room);
      if (accepted == 0) return 0;
      if (accepted < job.peers.size()) {
        job.peers.resize(accepted);
        job.bytes.resize(job.stride * accepted);
      }
      tx_pending_datagrams_ += accepted;
      tx_fanout_queue_.push_back(std::move(job));
    }
    if (tx_workers_.size() > 1) {
      tx_cv_.notify_all();
    } else {
      tx_cv_.notify_one();
    }
    return accepted;
  }

  bool queue_send_copy(const Conn& c, const uint8_t* data, size_t len) {
    return queue_send_copy_to(c, c.peer, data, len);
  }

  bool queue_send_copy_to(const Conn& c, const sockaddr_in& peer,
                          const uint8_t* data, size_t len) {
    std::vector<uint8_t> bytes = take_buffer();
    bytes.resize(len);
    std::memcpy(bytes.data(), data, len);
    Conn send_conn = c;
    send_conn.peer = peer;
    if (queue_send_owned(send_conn, std::move(bytes))) return true;
    recycle_buffer(std::move(bytes));
    return false;
  }

  void start_tx_worker() {
    if (!tx_workers_.empty()) return;
    tx_stop_ = false;
    size_t n = tx_worker_count_for(is_server_);
    tx_workers_.reserve(n);
    for (size_t i = 0; i < n; ++i) {
      tx_workers_.emplace_back([this]() { tx_loop(); });
    }
  }

  void stop_tx_worker() {
    {
      std::lock_guard<std::mutex> lock(tx_mu_);
      tx_stop_ = true;
      tx_queue_.clear();
      tx_fanout_queue_.clear();
      tx_pending_datagrams_ = 0;
    }
    tx_cv_.notify_all();
    for (std::thread& t : tx_workers_) {
      if (t.joinable()) t.join();
    }
    tx_workers_.clear();
  }

  void tx_loop() {
    std::vector<TxDatagram> batch;
    batch.reserve(TX_BATCH);
    while (true) {
      FanoutJob fanout;
      {
        std::unique_lock<std::mutex> lock(tx_mu_);
        tx_cv_.wait(lock, [&]() {
          return tx_stop_ || !tx_queue_.empty() || !tx_fanout_queue_.empty();
        });
        if (tx_stop_) return;
        if (!tx_fanout_queue_.empty()) {
          fanout = std::move(tx_fanout_queue_.front());
          tx_fanout_queue_.pop_front();
          tx_pending_datagrams_ -= fanout.peers.size();
        }
        while (fanout.peers.empty() && !tx_queue_.empty() && batch.size() < TX_BATCH) {
          batch.push_back(std::move(tx_queue_.front()));
          tx_queue_.pop_front();
          --tx_pending_datagrams_;
        }
      }
      if (!fanout.peers.empty()) {
        send_fanout_job(fanout);
        recycle_fanout_job(std::move(fanout));
      } else {
        send_batch(batch);
        recycle_worker_batch(batch);
        batch.clear();
      }
    }
  }

  void start_rx_worker() {
    if (rx_worker_.joinable()) return;
    rx_stop_.store(false, std::memory_order_release);
    rx_worker_ = std::thread([this]() { rx_loop(); });
  }

  void stop_rx_worker() {
    rx_stop_.store(true, std::memory_order_release);
    if (rx_worker_.joinable()) rx_worker_.join();
    std::lock_guard<std::mutex> lock(rx_mu_);
    rx_queue_.clear();
    rx_free_.clear();
  }

  void rx_loop() {
    std::array<std::array<uint8_t, MAX_UDP_PAYLOAD>, RECV_BATCH> bufs{};
    std::array<sockaddr_in, RECV_BATCH> srcs{};
    std::array<iovec, RECV_BATCH> iov{};
    std::array<mmsghdr, RECV_BATCH> msgs{};

    while (!rx_stop_.load(std::memory_order_acquire)) {
      for (size_t i = 0; i < RECV_BATCH; ++i) {
        iov[i].iov_base = bufs[i].data();
        iov[i].iov_len = bufs[i].size();
        std::memset(&msgs[i], 0, sizeof(msgs[i]));
        msgs[i].msg_hdr.msg_iov = &iov[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
        msgs[i].msg_hdr.msg_name = &srcs[i];
        msgs[i].msg_hdr.msg_namelen = sizeof(srcs[i]);
      }

      int n = ::recvmmsg(server_fd_, msgs.data(), RECV_BATCH, MSG_DONTWAIT, nullptr);
      if (n < 0) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          std::this_thread::sleep_for(std::chrono::microseconds(20));
          continue;
        }
        continue;
      }
      if (n == 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(20));
        continue;
      }

      std::lock_guard<std::mutex> lock(rx_mu_);
      for (int i = 0; i < n; ++i) {
        if (rx_queue_.size() >= MAX_RX_QUEUE) break;
        RxDatagram d;
        d.src = srcs[i];
        if (!rx_free_.empty()) {
          d.bytes = std::move(rx_free_.back());
          rx_free_.pop_back();
        }
        d.bytes.resize(msgs[i].msg_len);
        if (msgs[i].msg_len > 0) {
          std::memcpy(d.bytes.data(), bufs[i].data(), msgs[i].msg_len);
        }
        rx_queue_.push_back(std::move(d));
      }
    }
  }

  size_t send_batch(std::vector<TxDatagram>& batch) {
    if (batch.empty()) return 0;
    // main thread と複数 tx worker の双方から呼ばれるため、作業バッファは
    // thread_local で呼び出しをまたいで再利用する (メンバ化するとデータ競合)
    static thread_local std::vector<TxDatagram> physical;
    static thread_local std::vector<size_t> logical_counts;
    physical.clear();
    logical_counts.clear();
    physical.reserve(std::min(batch.size(), TX_BATCH));
    logical_counts.reserve(std::min(batch.size(), TX_BATCH));

    size_t idx = 0;
    while (idx < batch.size() && physical.size() < TX_BATCH) {
      int fd = send_fd_for(batch[idx]);
      const sockaddr_in peer = batch[idx].peer;
      size_t first_size = batch[idx].size();
      if (fd < 0) break;
      if (HEADER_BYTES + BATCH_FRAME_LEN_BYTES + first_size > MAX_UDP_PAYLOAD) {
        TxDatagram d;
        d.peer = peer;
        d.fd = fd;
        d.external = batch[idx].data();
        d.len = first_size;
        physical.push_back(std::move(d));
        logical_counts.push_back(1);
        ++idx;
        continue;
      }

      TxDatagram d;
      d.peer = peer;
      d.fd = fd;
      d.owned.resize(HEADER_BYTES);
      PacketHeader h;
      h.flags = FLAG_BATCH;
      write_header(d.owned.data(), h);

      size_t frames = 0;
      while (idx < batch.size() && send_fd_for(batch[idx]) == fd &&
             same_endpoint(batch[idx].peer, peer) &&
             d.owned.size() + BATCH_FRAME_LEN_BYTES + batch[idx].size() <=
                 MAX_UDP_PAYLOAD) {
        size_t offset = d.owned.size();
        size_t frame_len = batch[idx].size();
        d.owned.resize(offset + BATCH_FRAME_LEN_BYTES + frame_len);
        store_u16(d.owned.data() + offset, static_cast<uint16_t>(frame_len));
        std::memcpy(d.owned.data() + offset + BATCH_FRAME_LEN_BYTES,
                    batch[idx].data(), frame_len);
        ++idx;
        ++frames;
      }
      physical.push_back(std::move(d));
      logical_counts.push_back(frames);
    }

    size_t physical_sent = send_raw_batch(physical);
    size_t logical_sent = 0;
    for (size_t i = 0; i < physical_sent && i < logical_counts.size(); ++i) {
      logical_sent += logical_counts[i];
    }
    return logical_sent;
  }

  size_t send_raw_batch(std::vector<TxDatagram>& batch) {
    int fd = batch.empty() ? -1 : send_fd_for(batch.front());
    if (fd < 0 || batch.empty()) return 0;
    std::array<mmsghdr, TX_BATCH> msgs{};
    std::array<iovec, TX_BATCH> iov{};
    size_t n_msgs = 0;
    while (n_msgs < batch.size() && n_msgs < TX_BATCH &&
           send_fd_for(batch[n_msgs]) == fd) {
      ++n_msgs;
    }
    for (size_t i = 0; i < n_msgs; ++i) {
      iov[i].iov_base = const_cast<uint8_t*>(batch[i].data());
      iov[i].iov_len = batch[i].size();
      msgs[i].msg_hdr.msg_iov = &iov[i];
      msgs[i].msg_hdr.msg_iovlen = 1;
      msgs[i].msg_hdr.msg_name = &batch[i].peer;
      msgs[i].msg_hdr.msg_namelen = sizeof(batch[i].peer);
    }
    size_t offset = 0;
    while (offset < n_msgs) {
      int n = ::sendmmsg(fd, msgs.data() + offset,
                         static_cast<unsigned int>(n_msgs - offset),
                         MSG_DONTWAIT);
      if (n > 0) {
        offset += static_cast<size_t>(n);
        continue;
      }
      if (n < 0 && errno == EINTR) continue;
      break;
    }
    return offset;
  }

  size_t send_fanout_job(FanoutJob& job) {
    int fd = is_server_ ? server_fd_ : client_fd_;
    if (fd < 0 || job.peers.empty() || job.stride == 0) return 0;

    std::vector<TxDatagram> datagrams;
    datagrams.reserve(std::min(job.peers.size(), TX_BATCH));
    auto flush_datagrams = [&]() {
      while (!datagrams.empty()) {
        size_t sent = send_batch(datagrams);
        if (sent >= datagrams.size()) {
          datagrams.clear();
          return true;
        }
        if (sent == 0) return false;
        datagrams.erase(datagrams.begin(), datagrams.begin() + sent);
      }
      return true;
    };

    size_t idx = 0;
    size_t logical_sent = 0;
    while (idx < job.peers.size()) {
      const sockaddr_in peer = job.peers[idx];
      if (HEADER_BYTES + BATCH_FRAME_LEN_BYTES + job.stride > MAX_UDP_PAYLOAD) {
        TxDatagram d;
        d.peer = peer;
        d.fd = fd;
        d.external = job.bytes.data() + idx * job.stride;
        d.len = job.stride;
        datagrams.push_back(std::move(d));
        ++idx;
        ++logical_sent;
      } else {
        TxDatagram d;
        d.peer = peer;
        d.fd = fd;
        d.owned.resize(HEADER_BYTES);
        PacketHeader h;
        h.flags = FLAG_BATCH;
        write_header(d.owned.data(), h);

        size_t frames = 0;
        while (idx < job.peers.size() && same_endpoint(job.peers[idx], peer) &&
               d.owned.size() + BATCH_FRAME_LEN_BYTES + job.stride <= MAX_UDP_PAYLOAD) {
          size_t offset = d.owned.size();
          d.owned.resize(offset + BATCH_FRAME_LEN_BYTES + job.stride);
          store_u16(d.owned.data() + offset, static_cast<uint16_t>(job.stride));
          std::memcpy(d.owned.data() + offset + BATCH_FRAME_LEN_BYTES,
                      job.bytes.data() + idx * job.stride, job.stride);
          ++idx;
          ++frames;
        }
        datagrams.push_back(std::move(d));
        logical_sent += frames;
      }

      if (datagrams.size() >= TX_BATCH && !flush_datagrams()) return logical_sent;
    }
    flush_datagrams();
    return logical_sent;
  }

  void flush_direct_batch() {
    while (!direct_batch_.empty()) {
      size_t sent = send_batch(direct_batch_);
      if (sent >= direct_batch_.size()) {
        recycle_sent_datagrams(0, direct_batch_.size());
        direct_batch_.clear();
      } else if (sent > 0) {
        recycle_sent_datagrams(0, sent);
        direct_batch_.erase(direct_batch_.begin(), direct_batch_.begin() + sent);
      } else {
        break;
      }
    }
  }

  void recycle_sent_datagrams(size_t first, size_t last) {
    for (size_t i = first; i < last; ++i) {
      if (!direct_batch_[i].owned.empty()) {
        recycle_buffer(std::move(direct_batch_[i].owned));
      }
    }
  }

  void attach_ack(const Conn& c, PacketHeader* h, bool force) const {
    if (!force && !c.ack_dirty) return;
    if (!c.recv_window.have_ack()) return;
    h->flags |= FLAG_HAS_ACK;
    h->ack = c.recv_window.ack();
    h->ack_bits = c.recv_window.ack_bits();
  }

  void note_ack_sent(Conn& c, const PacketHeader& h) {
    if ((h.flags & FLAG_HAS_ACK) != 0) {
      c.ack_dirty = false;
      c.ack_due = clock_type::time_point::max();
    }
  }

  // conns_ の要素であることが前提 (id == index + 1 は client/server 共通の不変条件)
  uint32_t conn_id_of(const Conn& c) const {
    return static_cast<uint32_t>(&c - conns_.data()) + 1;
  }

  void add_to_retx_list(Conn& c) {
    if (c.in_retx_list) return;
    c.in_retx_list = true;
    retx_list_.push_back(conn_id_of(c));
  }

  void mark_ack_dirty(Conn& c, clock_type::time_point now) {
    if (!c.ack_dirty) {
      c.ack_dirty = true;
      c.ack_due = now + ack_delay();
    }
    if (!c.in_ack_list) {
      c.in_ack_list = true;
      ack_list_.push_back(conn_id_of(c));
    }
    if (c.ack_due < earliest_ack_) earliest_ack_ = c.ack_due;
  }

  // RFC 6298 相当の SRTT/RTTVAR 更新
  void update_rtt(Conn& c, clock_type::duration sample) {
    if (sample <= clock_type::duration::zero()) {
      sample = clock_type::duration(1);
    }
    if (!c.rtt_valid) {
      c.srtt = sample;
      c.rttvar = sample / 2;
      c.rtt_valid = true;
    } else {
      auto diff = c.srtt > sample ? c.srtt - sample : sample - c.srtt;
      c.rttvar = (c.rttvar * 3 + diff) / 4;
      c.srtt = (c.srtt * 7 + sample) / 8;
    }
    clock_type::duration rto =
        c.srtt + std::max<clock_type::duration>(RTO_GRANULARITY, c.rttvar * 4);
    c.rto = std::clamp<clock_type::duration>(rto, MIN_RTO, MAX_RTO);
  }

  // 再送タイマ: 適応 RTO × 指数バックオフ (2^retx_count, 上限あり)
  clock_type::duration retx_delay(const Conn& c, const PendingSend& p) const {
    clock_type::duration d = c.rto;
    const unsigned shift = std::min<unsigned>(p.retx_count, 5);
    d *= (1u << shift);
    return std::min<clock_type::duration>(d, MAX_RETX_BACKOFF);
  }

  // pending 1 個をワイヤに出す (初回送出・タイマ再送・fast retransmit 共通)。
  // 送出できたら sent_at 等を更新して true。
  bool transmit_pending(Conn& c, PendingSend& p, clock_type::time_point now) {
    PacketHeader h = parse_header(p.bytes.data());
    h.flags = FLAG_REL;
    attach_ack(c, &h, true);
    write_header(p.bytes.data(), h);
    if (p.transmitted) {
      // 再送: direct_batch_ に未送出の旧参照が残っていれば重複送出を避ける
      drop_direct_ref(p.bytes.data());
    }
    if (!queue_send_ref(c, p.bytes.data(), p.bytes.size())) return false;
    note_ack_sent(c, h);
    if (!p.transmitted) {
      p.transmitted = true;
      p.first_sent_at = now;
    } else if (p.retx_count < 255) {
      ++p.retx_count;
    }
    p.sent_at = now;
    p.skip_acks = 0;
    return true;
  }

  // ACK で窓 (未ACK最小 seq) が進んだ後、新たに窓内に入った未送出 pending を
  // 送出する。pending は seq 昇順なので窓外に達したら打ち切ってよい。
  void pump_pending(Conn& c, clock_type::time_point now) {
    if (c.pending.empty()) return;
    const uint32_t base = c.pending.front().seq;
    for (PendingSend& p : c.pending) {
      if (p.transmitted) continue;
      if (p.seq - base >= SEND_WINDOW) break;
      if (!transmit_pending(c, p, now)) {
        // TX バックプレッシャ。service_retransmits で再試行させる
        schedule_retx(now + TX_RETRY_DELAY);
        break;
      }
      schedule_retx(p.sent_at + retx_delay(c, p));
    }
  }

  void process_ack(Conn& c, uint32_t ack, uint64_t ack_bits,
                   clock_type::time_point now) {
    if (ack == 0 || c.pending.empty()) return;
    bool advanced = false;
    for (auto it = c.pending.begin(); it != c.pending.end();) {
      if (ack_covers(ack, ack_bits, it->seq)) {
        // Karn のアルゴリズム: 再送済みパケットの ACK は RTT サンプルに使わない
        if (it->transmitted && it->retx_count == 0) {
          update_rtt(c, now - it->sent_at);
        }
        // direct_batch_ に未送出の参照が残ったまま recycle すると use-after-free
        // になるため、必ず先に参照を除去する
        drop_direct_ref(it->bytes.data());
        recycle_buffer(std::move(it->bytes));
        it = c.pending.erase(it);
        advanced = true;
        continue;
      }
      // SACK の穴: この seq を飛ばして先の seq が ACK されている。閾値に達したら
      // タイマを待たず fast retransmit する
      if (it->transmitted && seq32_after(ack, it->seq)) {
        if (it->skip_acks < FAST_RETX_SKIPS) ++it->skip_acks;
        if (it->skip_acks >= FAST_RETX_SKIPS && transmit_pending(c, *it, now)) {
          schedule_retx(it->sent_at + retx_delay(c, *it));
        }
      }
      ++it;
    }
    if (advanced) pump_pending(c, now);
  }

  void process_packet(const uint8_t* pkt, size_t len, const sockaddr_in& src,
                      clock_type::time_point now) {
    if (len < HEADER_BYTES) return;
    PacketHeader h = parse_header(pkt);
    if ((h.flags & FLAG_BATCH) != 0) {
      process_batch_packet(pkt + HEADER_BYTES, len - HEADER_BYTES, src, now);
      return;
    }

    Conn* c = nullptr;
    uint32_t out_id = 0;
    bool has_payload = (h.flags & FLAG_ACK_ONLY) == 0;
    if (is_server_) {
      c = server_conn_for(src, h.conv, has_payload);
      if (!c) return;
      out_id = static_cast<uint32_t>(c - conns_.data()) + 1;
      c->peer = src;
    } else {
      c = find_conn(h.conv);
      if (!c) return;
      out_id = h.conv;
    }

    if ((h.flags & FLAG_HAS_ACK) != 0) {
      process_ack(*c, h.ack, h.ack_bits, now);
    }
    if ((h.flags & FLAG_ACK_ONLY) != 0) return;

    const uint8_t* payload = pkt + HEADER_BYTES;
    size_t payload_len = len - HEADER_BYTES;
    if ((h.flags & FLAG_REL) != 0) {
      bool first_delivery = c->recv_window.insert(h.seq);
      mark_ack_dirty(*c, now);
      if (!first_delivery) return;
    }
    inbox_.enqueue(out_id, payload, payload_len);
  }

  void process_batch_packet(const uint8_t* body, size_t len,
                            const sockaddr_in& src, clock_type::time_point now) {
    if (!is_server_) batch_recv_seen_ = true;
    size_t offset = 0;
    while (offset + BATCH_FRAME_LEN_BYTES <= len) {
      uint16_t frame_len = load_u16(body + offset);
      offset += BATCH_FRAME_LEN_BYTES;
      if (frame_len < HEADER_BYTES || offset + frame_len > len) return;
      // batch フレームのネスト (フレーム内 FLAG_BATCH) は送信側が生成しない
      // 不正入力。再帰させず datagram ごと破棄する
      if ((body[offset] & FLAG_BATCH) != 0) return;
      process_packet(body + offset, frame_len, src, now);
      offset += frame_len;
    }
  }

  void drain_socket(size_t max_packets) {
    int fd = is_server_ ? server_fd_ : client_fd_;
    if (fd < 0 || max_packets == 0) return;
    if (rx_worker_.joinable()) {
      drain_rx_queue(max_packets);
      return;
    }
    if (!is_server_ && !client_fds_.empty()) {
      size_t per_fd = std::max<size_t>(1, max_packets / client_fds_.size());
      for (int client_fd : client_fds_) {
        drain_fd_packets(client_fd, per_fd);
      }
      return;
    }
    drain_fd_packets(fd, max_packets);
    if (server_ack_fd_ >= 0) {
      drain_fd_packets(server_ack_fd_, max_packets);
    }
  }

  void drain_fd_packets(int fd, size_t max_packets) {
    size_t drained = 0;
    while (drained < max_packets) {
      size_t batch = std::min(RECV_BATCH, max_packets - drained);
      for (size_t i = 0; i < batch; ++i) {
        iov_[i].iov_base = recv_bufs_[i].data();
        iov_[i].iov_len = recv_bufs_[i].size();
        std::memset(&msgs_[i], 0, sizeof(msgs_[i]));
        msgs_[i].msg_hdr.msg_iov = &iov_[i];
        msgs_[i].msg_hdr.msg_iovlen = 1;
        msgs_[i].msg_hdr.msg_name = &srcs_[i];
        msgs_[i].msg_hdr.msg_namelen = sizeof(srcs_[i]);
      }

      int n = ::recvmmsg(fd, msgs_.data(), static_cast<unsigned int>(batch),
                         MSG_DONTWAIT, nullptr);
      if (n < 0) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        break;
      }
      if (n == 0) break;

      auto now = clock_type::now();
      for (int i = 0; i < n; ++i) {
        process_packet(recv_bufs_[i].data(), msgs_[i].msg_len, srcs_[i], now);
      }
      drained += static_cast<size_t>(n);
      if (static_cast<size_t>(n) < batch) break;
    }
  }

  void drain_rx_queue(size_t max_packets) {
    size_t drained = 0;
    std::vector<std::vector<uint8_t>> recycle;
    recycle.reserve(RECV_BATCH);
    while (drained < max_packets) {
      RxDatagram d;
      {
        std::lock_guard<std::mutex> lock(rx_mu_);
        if (rx_queue_.empty()) break;
        d = std::move(rx_queue_.front());
        rx_queue_.pop_front();
      }
      auto now = clock_type::now();
      process_packet(d.bytes.data(), d.bytes.size(), d.src, now);
      recycle.push_back(std::move(d.bytes));
      if (recycle.size() >= RECV_BATCH) {
        recycle_rx_buffers(std::move(recycle));
        recycle.clear();
      }
      ++drained;
    }
    if (!recycle.empty()) recycle_rx_buffers(std::move(recycle));
  }

  void recycle_rx_buffers(std::vector<std::vector<uint8_t>>&& buffers) {
    std::lock_guard<std::mutex> lock(rx_mu_);
    for (auto& b : buffers) {
      b.clear();
      rx_free_.push_back(std::move(b));
    }
  }

  // pending を持つ Conn (retx_list_) だけを走査する。全 Conn の線形走査は
  // 1000conn 級で O(N) が高頻度に効くため避ける。
  void service_retransmits(clock_type::time_point now) {
    auto next_due = clock_type::time_point::max();
    size_t keep = 0;
    for (size_t i = 0; i < retx_list_.size(); ++i) {
      const uint32_t id = retx_list_[i];
      Conn& c = conns_[id - 1];
      if (!c.active || c.pending.empty()) {
        c.in_retx_list = false;
        continue;
      }
      bool conn_timed_out = false;
      const uint32_t base = c.pending.front().seq;
      for (PendingSend& p : c.pending) {
        if (!p.transmitted) {
          // 初回送出は in-flight 窓内のみ。pending は seq 昇順 かつ
          // transmitted は前方に寄るので、窓外に達したら打ち切ってよい
          if (p.seq - base >= SEND_WINDOW) break;
          if (!transmit_pending(c, p, now)) {
            if (now + TX_RETRY_DELAY < next_due) next_due = now + TX_RETRY_DELAY;
            break;
          }
          auto due = p.sent_at + retx_delay(c, p);
          if (due < next_due) next_due = due;
          continue;
        }
        // reliable_timeout は初回送出からの経過で判定 (未送出分は対象外)
        if (now - p.first_sent_at >= reliable_timeout_) {
          conn_timed_out = true;
          break;
        }
        auto due = p.sent_at + retx_delay(c, p);
        if (now >= due) {
          if (transmit_pending(c, p, now)) {
            due = p.sent_at + retx_delay(c, p);
          } else {
            due = now + TX_RETRY_DELAY;
          }
        }
        if (due < next_due) next_due = due;
      }
      if (conn_timed_out) {
        expire_conn(c);
        c.in_retx_list = false;
        continue;
      }
      retx_list_[keep++] = id;
    }
    retx_list_.resize(keep);
    earliest_retx_ = next_due;
  }

  void expire_conn(Conn& c) {
    if (!c.active) return;
    c.active = false;
    drop_direct_refs_for(c);
    for (PendingSend& p : c.pending) {
      recycle_buffer(std::move(p.bytes));
    }
    c.pending.clear();
    c.ack_dirty = false;
    c.ack_due = clock_type::time_point::max();
    if (active_conns_ > 0) --active_conns_;
    ++shutdown_by_transport_;
  }

  // 不変条件: direct_batch_ は pending.bytes への生ポインタ (TxDatagram.external)
  // を保持しうる。pending の buffer を recycle/解放する前に、必ず
  // drop_direct_ref() / drop_direct_refs_for() で参照を除去すること
  // (process_ack / expire_conn / transmit_pending が該当)。
  // recycle_buffer() のデバッグ assert がこの不変条件を検査する。
  void drop_direct_ref(const uint8_t* pending_data) {
    if (direct_batch_.empty()) return;
    direct_batch_.erase(
        std::remove_if(direct_batch_.begin(), direct_batch_.end(),
                       [&](const TxDatagram& d) {
                         return d.external == pending_data;
                       }),
        direct_batch_.end());
  }

  void drop_direct_refs_for(const Conn& c) {
    for (const PendingSend& p : c.pending) {
      drop_direct_ref(p.bytes.data());
    }
  }

  // ack_dirty な Conn (ack_list_) だけを走査する
  void service_standalone_acks(clock_type::time_point now) {
    auto next_due = clock_type::time_point::max();
    std::array<uint8_t, HEADER_BYTES> pkt{};
    size_t keep = 0;
    for (size_t i = 0; i < ack_list_.size(); ++i) {
      const uint32_t id = ack_list_[i];
      Conn& c = conns_[id - 1];
      if (!c.active || !c.ack_dirty) {
        // piggyback (note_ack_sent) で送信済み、または expire 済み
        c.in_ack_list = false;
        continue;
      }
      if (now < c.ack_due) {
        if (c.ack_due < next_due) next_due = c.ack_due;
        ack_list_[keep++] = id;
        continue;
      }
      PacketHeader h;
      h.flags = FLAG_ACK_ONLY;
      h.conv = c.conv;
      attach_ack(c, &h, true);
      if ((h.flags & FLAG_HAS_ACK) == 0) {
        c.ack_dirty = false;
        c.in_ack_list = false;
        continue;
      }
      write_header(pkt.data(), h);
      const sockaddr_in& ack_peer =
          (!is_server_ && split_ack_enabled()) ? server_ack_peer_ : c.peer;
      if (queue_send_copy_to(c, ack_peer, pkt.data(), pkt.size())) {
        note_ack_sent(c, h);
        c.in_ack_list = false;
      } else {
        c.ack_due = now + ack_delay();
        if (c.ack_due < next_due) next_due = c.ack_due;
        ack_list_[keep++] = id;
      }
    }
    ack_list_.resize(keep);
    earliest_ack_ = next_due;
  }

  std::vector<uint8_t> take_buffer() {
    if (free_buffers_.empty()) collect_recycled_buffers();
    if (free_buffers_.empty()) return std::vector<uint8_t>();
    std::vector<uint8_t> b = std::move(free_buffers_.back());
    free_buffers_.pop_back();
    b.clear();
    return b;
  }

  void recycle_buffer(std::vector<uint8_t>&& b) {
#ifndef NDEBUG
    // 不変条件の検査: direct_batch_ が参照している buffer を recycle しては
    // ならない (drop_direct_ref を先に呼ぶこと)。違反すると再利用時に
    // use-after-free 相当の内容破壊になる
    for (const TxDatagram& d : direct_batch_) {
      assert(d.external != b.data());
    }
#endif
    b.clear();
    free_buffers_.push_back(std::move(b));
  }

  FanoutJob take_fanout_job() {
    std::lock_guard<std::mutex> lock(recycled_mu_);
    if (fanout_pool_.empty()) return FanoutJob{};
    FanoutJob job = std::move(fanout_pool_.back());
    fanout_pool_.pop_back();
    job.bytes.clear();
    job.peers.clear();
    job.stride = 0;
    return job;
  }

  void recycle_fanout_job(FanoutJob&& job) {
    job.bytes.clear();
    job.peers.clear();
    job.stride = 0;
    std::lock_guard<std::mutex> lock(recycled_mu_);
    if (fanout_pool_.size() < 64) {
      fanout_pool_.push_back(std::move(job));
    }
  }

  void recycle_worker_batch(std::vector<TxDatagram>& batch) {
    std::vector<std::vector<uint8_t>> recycled;
    recycled.reserve(batch.size());
    for (TxDatagram& d : batch) {
      if (!d.owned.empty()) {
        d.owned.clear();
        recycled.push_back(std::move(d.owned));
      }
    }
    if (recycled.empty()) return;
    std::lock_guard<std::mutex> lock(recycled_mu_);
    for (auto& b : recycled) {
      recycled_buffers_.push_back(std::move(b));
    }
  }

  void collect_recycled_buffers() {
    std::lock_guard<std::mutex> lock(recycled_mu_);
    while (!recycled_buffers_.empty()) {
      free_buffers_.push_back(std::move(recycled_buffers_.back()));
      recycled_buffers_.pop_back();
    }
  }

  void schedule_retx(clock_type::time_point due) {
    if (due < earliest_retx_) earliest_retx_ = due;
  }

  int server_fd_ = -1;
  int server_ack_fd_ = -1;
  int client_fd_ = -1;
  std::vector<int> client_fds_;
  bool is_server_ = false;
  bool batch_recv_seen_ = false;
  sockaddr_in server_peer_{};
  sockaddr_in server_ack_peer_{};
  std::vector<Conn> conns_;
  std::unordered_map<uint64_t, Peer> peers_;
  // dirty リスト: 全 Conn 線形走査を避けるため、ack 待ち / pending ありの
  // Conn id だけを保持する (in_ack_list / in_retx_list フラグと対)
  std::vector<uint32_t> ack_list_;
  std::vector<uint32_t> retx_list_;
  rudp_bench::ReusableInboundQueue inbox_;
  std::vector<std::vector<uint8_t>> free_buffers_;
  std::mutex recycled_mu_;
  std::vector<std::vector<uint8_t>> recycled_buffers_;
  std::vector<FanoutJob> fanout_pool_;  // recycled_mu_ 保護
  std::vector<TxDatagram> direct_batch_;
  std::vector<Conn*> fanout_conns_;
  std::vector<PacketHeader> fanout_headers_;
  std::mutex tx_mu_;
  std::condition_variable tx_cv_;
  std::deque<TxDatagram> tx_queue_;
  std::deque<FanoutJob> tx_fanout_queue_;
  size_t tx_pending_datagrams_ = 0;
  std::vector<std::thread> tx_workers_;
  bool tx_stop_ = false;
  std::mutex rx_mu_;
  std::deque<RxDatagram> rx_queue_;
  std::vector<std::vector<uint8_t>> rx_free_;
  std::thread rx_worker_;
  std::atomic<bool> rx_stop_{false};
  uint32_t next_id_ = 1;
  uint32_t connected_peak_ = 0;
  uint32_t shutdown_by_transport_ = 0;
  uint32_t hinted_connections_ = 0;
  uint32_t active_conns_ = 0;
  // env はインスタンス生成時に一度だけ読む (ホットパスから getenv を排除)
  const std::chrono::milliseconds reliable_timeout_ = reliable_timeout_from_env();
  const uint32_t conn_limit_env_ = conn_limit_from_env();
  clock_type::time_point earliest_ack_ = clock_type::time_point::max();
  clock_type::time_point earliest_retx_ = clock_type::time_point::max();
  size_t recv_empty_drains_left_ = 0;
  size_t client_recv_budget_left_ = std::numeric_limits<size_t>::max();

  std::array<std::array<uint8_t, MAX_UDP_PAYLOAD>, RECV_BATCH> recv_bufs_{};
  std::array<sockaddr_in, RECV_BATCH> srcs_{};
  std::array<iovec, RECV_BATCH> iov_{};
  std::array<mmsghdr, RECV_BATCH> msgs_{};
};

}  // namespace

namespace rudp_bench {
void register_apex_rudp_adapter() {
  register_adapter("apex_rudp", []() { return std::make_unique<ApexRudpAdapter>(); });
}
}  // namespace rudp_bench
