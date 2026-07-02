#include "coop_rudp/rudp.h"
#include "harness/adapter.h"
#include "harness/adapter_registry.h"
#include "harness/socket_buffer.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr uint16_t DEFAULT_MTU = 1200;
// ワイヤ定数はコア（rudp.h）の公開定義を使う。二重定義するとコアの
// フォーマット変更がサイレントに adapter を壊す。
constexpr size_t MAX_ADAPTER_PAYLOAD_BYTES =
    DEFAULT_MTU - RUDP_WIRE_PACKET_HEADER_BYTES -
    RUDP_WIRE_DATA_FRAME_HEADER_BYTES;
constexpr int UDP_SOCKET_BUFFER_BYTES = 4 * 1024 * 1024;
constexpr size_t IO_BATCH_MAX = 256;
constexpr size_t PHYSICAL_MTU = 65507;
constexpr uint32_t PHYSICAL_BATCH_MAGIC = 0x31425243u;  // "CRB1" little-endian.
constexpr size_t PHYSICAL_BATCH_HEADER = 4;
constexpr size_t PHYSICAL_BATCH_LEN = 2;
constexpr size_t ASYNC_TX_PACKET_LIMIT = 65536;
constexpr size_t ASYNC_TX_BYTE_LIMIT = 64u * 1024u * 1024u;
constexpr size_t RX_PENDING_PACKET_LIMIT = 65536;
constexpr size_t RX_PENDING_BYTE_LIMIT = 64u * 1024u * 1024u;
constexpr uint32_t MAX_ADAPTER_CONNECTIONS = 4096;
constexpr uint32_t MESSAGES_PER_CONNECTION = 64;
constexpr uint32_t BROAD_RECV_EVENTS_PER_CONNECTION = 64;
constexpr uint32_t NARROW_RECV_EVENTS_PER_CONNECTION = 16;
constexpr uint64_t DEFAULT_IDLE_POLL_MIN_NS = 100'000;
constexpr uint32_t DEFAULT_MAX_RETRANSMITS = 64;
constexpr uint32_t DEFAULT_IDLE_TIMEOUT_MS = 10'000;

using clock_type = std::chrono::steady_clock;

struct AsyncTxPacket {
  rudp_addr addr{};
  std::vector<uint8_t> bytes;
};

struct SockCtx {
  int fd = -1;
  // Clock cache refreshed once per adapter entry point. rudp_send reads the
  // socket clock per call, which dominates CPU during fanout (one read per
  // recipient) if it hits the real clock every time.
  uint64_t cached_now_ns = 0;
  bool async_tx = false;
  bool tx_stop = false;
  std::mutex tx_mu;
  std::condition_variable tx_cv;
  std::deque<AsyncTxPacket> tx_queue;
  // 送信済みバッファの再利用プール（tx_mu 保護）。パケット毎の
  // std::vector heap 確保が高スループット時のアロケーション churn に
  // なるのを避ける。
  std::vector<std::vector<uint8_t>> tx_free;
  size_t tx_queue_bytes = 0;
  std::deque<AsyncTxPacket> rx_pending;
  std::vector<AsyncTxPacket> rx_pending_delivered;
  size_t rx_pending_bytes = 0;
  std::vector<uint8_t> tx_phys_data;
  std::vector<uint8_t> rx_phys_data;
  std::thread tx_thread;
};

void set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void tune_socket_buffers(int fd, const char* socket_role) {
  rudp_bench::tune_udp_socket_buffers(fd, UDP_SOCKET_BUFFER_BYTES,
                                      UDP_SOCKET_BUFFER_BYTES, "coop_rudp",
                                      socket_role);
}

rudp_addr to_rudp_addr(const sockaddr_in& in) {
  rudp_addr a{};
  a.len = sizeof(sockaddr_in);
  std::memcpy(a.data, &in, sizeof(sockaddr_in));
  return a;
}

sockaddr_in to_sockaddr(const rudp_addr& a) {
  sockaddr_in out{};
  if (a.len == sizeof(sockaddr_in)) {
    std::memcpy(&out, a.data, sizeof(sockaddr_in));
  }
  return out;
}

bool same_rudp_addr(const rudp_addr& a, const rudp_addr& b) {
  if (a.len > sizeof(a.data) || b.len > sizeof(b.data)) return false;
  return a.len == b.len && std::memcmp(a.data, b.data, a.len) == 0;
}

void store_u16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v & 0xffu);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xffu);
}

uint16_t load_u16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(p[1]) << 8);
}

void store_u32(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xffu);
  p[1] = static_cast<uint8_t>((v >> 8) & 0xffu);
  p[2] = static_cast<uint8_t>((v >> 16) & 0xffu);
  p[3] = static_cast<uint8_t>((v >> 24) & 0xffu);
}

uint32_t load_u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

bool rudp_packet_can_enter_core(const uint8_t* data, size_t len) {
  uint64_t conn_id = 0;
  if (!rudp_packet_precheck(data, len, DEFAULT_MTU, &conn_id)) return false;
  // コアの conn_id は 64bit だが、この adapter は harness の 32bit conn_id を
  // そのままワイヤ ID に使う設計のため 32bit 超を境界で弾く（adapter 都合の
  // 制限であって、コア API は 64bit のまま）。
  return conn_id <= std::numeric_limits<uint32_t>::max();
}

void ensure_physical_buffers(SockCtx* ctx) {
  if (ctx->tx_phys_data.size() != IO_BATCH_MAX * PHYSICAL_MTU) {
    ctx->tx_phys_data.resize(IO_BATCH_MAX * PHYSICAL_MTU);
  }
  if (ctx->rx_phys_data.size() != IO_BATCH_MAX * PHYSICAL_MTU) {
    ctx->rx_phys_data.resize(IO_BATCH_MAX * PHYSICAL_MTU);
  }
}

bool coop_async_tx_enabled() {
  const char* v = std::getenv("COOP_ASYNC_TX");
  return v && *v && std::strcmp(v, "0") != 0;
}

size_t size_env_or_default(const char* name, size_t default_value) {
  const char* v = std::getenv(name);
  if (!v || !*v) return default_value;
  errno = 0;
  char* end = nullptr;
  unsigned long long parsed = std::strtoull(v, &end, 10);
  if (end == v || *end != '\0' || errno == ERANGE || parsed == 0) {
    return default_value;
  }
  size_t max_size = std::numeric_limits<size_t>::max();
  if (parsed > max_size) return max_size;
  return static_cast<size_t>(parsed);
}

uint32_t u32_env_or_default(const char* name, uint32_t default_value) {
  const char* v = std::getenv(name);
  if (!v || !*v) return default_value;
  errno = 0;
  char* end = nullptr;
  unsigned long long parsed = std::strtoull(v, &end, 10);
  if (end == v || *end != '\0' || errno == ERANGE) return default_value;
  if (parsed > std::numeric_limits<uint32_t>::max()) {
    return std::numeric_limits<uint32_t>::max();
  }
  return static_cast<uint32_t>(parsed);
}

uint64_t coop_idle_poll_min_ns() {
  static uint64_t value = []() -> uint64_t {
    const char* v = std::getenv("COOP_IDLE_POLL_US");
    if (!v || !*v) return DEFAULT_IDLE_POLL_MIN_NS;
    errno = 0;
    char* end = nullptr;
    unsigned long long parsed = std::strtoull(v, &end, 10);
    if (end == v || *end != '\0' || errno == ERANGE) {
      return DEFAULT_IDLE_POLL_MIN_NS;
    }
    if (parsed > std::numeric_limits<uint64_t>::max() / 1000ull) {
      return std::numeric_limits<uint64_t>::max();
    }
    return static_cast<uint64_t>(parsed) * 1000ull;
  }();
  return value;
}

int send_packets_raw_now(int fd, const rudp_out_packet* packets, size_t count) {
  if (fd < 0) return -1;
  std::array<mmsghdr, IO_BATCH_MAX> msgs{};
  std::array<iovec, IO_BATCH_MAX> iov{};
  std::array<sockaddr_in, IO_BATCH_MAX> addrs{};
  size_t n_msgs = std::min(count, IO_BATCH_MAX);
  for (size_t i = 0; i < n_msgs; ++i) {
    addrs[i] = to_sockaddr(packets[i].addr);
    iov[i].iov_base = const_cast<uint8_t*>(packets[i].data);
    iov[i].iov_len = packets[i].len;
    msgs[i].msg_hdr.msg_iov = &iov[i];
    msgs[i].msg_hdr.msg_iovlen = 1;
    msgs[i].msg_hdr.msg_name = &addrs[i];
    msgs[i].msg_hdr.msg_namelen = sizeof(addrs[i]);
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
  return static_cast<int>(offset);
}

struct PhysicalPacket {
  rudp_addr addr{};
  uint8_t* data = nullptr;
  size_t len = 0;
  size_t logical_count = 0;
};

int send_packets_batched_now(SockCtx* ctx, const rudp_out_packet* packets,
                             size_t count) {
  if (!ctx || ctx->fd < 0) return -1;
  ensure_physical_buffers(ctx);
  std::array<PhysicalPacket, IO_BATCH_MAX> physical{};
  std::array<rudp_out_packet, IO_BATCH_MAX> out{};
  size_t physical_count = 0;

  for (size_t i = 0; i < count;) {
    if (packets[i].len + PHYSICAL_BATCH_HEADER + PHYSICAL_BATCH_LEN > PHYSICAL_MTU) {
      break;
    }
    if (physical_count >= IO_BATCH_MAX) break;
    PhysicalPacket& pp = physical[physical_count++];
    pp.addr = packets[i].addr;
    pp.data = ctx->tx_phys_data.data() + (physical_count - 1) * PHYSICAL_MTU;
    pp.len = PHYSICAL_BATCH_HEADER;
    pp.logical_count = 0;
    store_u32(pp.data, PHYSICAL_BATCH_MAGIC);
    while (i < count && same_rudp_addr(pp.addr, packets[i].addr) &&
           pp.len + PHYSICAL_BATCH_LEN + packets[i].len <= PHYSICAL_MTU) {
      store_u16(pp.data + pp.len, static_cast<uint16_t>(packets[i].len));
      pp.len += PHYSICAL_BATCH_LEN;
      std::memcpy(pp.data + pp.len, packets[i].data, packets[i].len);
      pp.len += packets[i].len;
      ++pp.logical_count;
      ++i;
    }
  }

  for (size_t i = 0; i < physical_count; ++i) {
    out[i].addr = physical[i].addr;
    out[i].data = physical[i].data;
    out[i].len = physical[i].len;
  }
  int sent_physical = send_packets_raw_now(ctx->fd, out.data(), physical_count);
  if (sent_physical < 0) return sent_physical;
  size_t sent_logical = 0;
  for (int i = 0; i < sent_physical; ++i) sent_logical += physical[i].logical_count;
  return static_cast<int>(sent_logical);
}

int enqueue_async_tx(SockCtx* ctx, const rudp_out_packet* packets, size_t count) {
  if (!ctx || !packets) return -1;
  size_t accepted = 0;
  size_t packet_limit =
      size_env_or_default("COOP_ASYNC_TX_PACKET_LIMIT", ASYNC_TX_PACKET_LIMIT);
  size_t byte_limit =
      size_env_or_default("COOP_ASYNC_TX_BYTE_LIMIT", ASYNC_TX_BYTE_LIMIT);
  {
    std::lock_guard<std::mutex> lock(ctx->tx_mu);
    if (ctx->tx_stop) return -1;
    for (size_t i = 0; i < count; ++i) {
      if (!packets[i].data && packets[i].len != 0) {
        return accepted == 0 ? -1 : static_cast<int>(accepted);
      }
      if (ctx->tx_queue.size() >= packet_limit) break;
      if (ctx->tx_queue_bytes >= byte_limit) break;
      if (packets[i].len > byte_limit - ctx->tx_queue_bytes) break;
      AsyncTxPacket p;
      if (!ctx->tx_free.empty()) {
        p.bytes = std::move(ctx->tx_free.back());
        ctx->tx_free.pop_back();
      }
      p.addr = packets[i].addr;
      p.bytes.resize(packets[i].len);
      if (packets[i].len != 0) {
        std::memcpy(p.bytes.data(), packets[i].data, packets[i].len);
      }
      ctx->tx_queue_bytes += packets[i].len;
      ctx->tx_queue.push_back(std::move(p));
      ++accepted;
    }
  }
  if (accepted > 0) ctx->tx_cv.notify_one();
  return static_cast<int>(accepted);
}

int send_batch_cb(void* user, const rudp_out_packet* packets, size_t count) {
  auto* ctx = static_cast<SockCtx*>(user);
  if (!ctx || ctx->fd < 0) return -1;
  if (ctx->async_tx) return enqueue_async_tx(ctx, packets, count);
  return send_packets_batched_now(ctx, packets, count);
}

bool enqueue_pending_rx(SockCtx* ctx, const rudp_addr& src, const uint8_t* bytes,
                        size_t n) {
  if (!ctx || (!bytes && n != 0)) return false;
  if (ctx->rx_pending.size() >= RX_PENDING_PACKET_LIMIT) return false;
  if (ctx->rx_pending_bytes >= RX_PENDING_BYTE_LIMIT) return false;
  if (n > RX_PENDING_BYTE_LIMIT - ctx->rx_pending_bytes) return false;
  AsyncTxPacket pending;
  pending.addr = src;
  pending.bytes.resize(n);
  if (n != 0) std::memcpy(pending.bytes.data(), bytes, n);
  ctx->rx_pending_bytes += n;
  ctx->rx_pending.push_back(std::move(pending));
  return true;
}

void deliver_physical_packet(SockCtx* ctx, const rudp_addr& src, const uint8_t* data,
                             size_t len, rudp_in_packet* packets, size_t max_count,
                             size_t* out_count) {
  auto deliver_one = [&](const uint8_t* bytes, size_t n) {
    if (!rudp_packet_can_enter_core(bytes, n)) return;
    if (*out_count < max_count) {
      packets[*out_count].addr = src;
      packets[*out_count].data = const_cast<uint8_t*>(bytes);
      packets[*out_count].cap = n;
      packets[*out_count].len = n;
      ++(*out_count);
      return;
    }
    (void)enqueue_pending_rx(ctx, src, bytes, n);
  };

  if (len < PHYSICAL_BATCH_HEADER || load_u32(data) != PHYSICAL_BATCH_MAGIC) {
    deliver_one(data, len);
    return;
  }

  size_t off = PHYSICAL_BATCH_HEADER;
  size_t frames = 0;
  while (off + PHYSICAL_BATCH_LEN <= len) {
    uint16_t frame_len = load_u16(data + off);
    off += PHYSICAL_BATCH_LEN;
    if (frame_len == 0 || off + frame_len > len) {
      deliver_one(data, len);
      return;
    }
    off += frame_len;
    ++frames;
  }
  if (frames == 0 || off != len) {
    deliver_one(data, len);
    return;
  }

  off = PHYSICAL_BATCH_HEADER;
  while (off + PHYSICAL_BATCH_LEN <= len) {
    uint16_t frame_len = load_u16(data + off);
    off += PHYSICAL_BATCH_LEN;
    deliver_one(data + off, frame_len);
    off += frame_len;
  }
}

size_t drain_pending_rx(SockCtx* ctx, rudp_in_packet* packets, size_t max_count) {
  size_t n = 0;
  ctx->rx_pending_delivered.clear();
  ctx->rx_pending_delivered.reserve(max_count);
  while (n < max_count && !ctx->rx_pending.empty()) {
    size_t pending_bytes = ctx->rx_pending.front().bytes.size();
    ctx->rx_pending_delivered.push_back(std::move(ctx->rx_pending.front()));
    ctx->rx_pending.pop_front();
    if (ctx->rx_pending_bytes >= pending_bytes) {
      ctx->rx_pending_bytes -= pending_bytes;
    } else {
      ctx->rx_pending_bytes = 0;
    }
    AsyncTxPacket& p = ctx->rx_pending_delivered.back();
    packets[n].addr = p.addr;
    packets[n].data = p.bytes.data();
    packets[n].cap = p.bytes.size();
    packets[n].len = p.bytes.size();
    ++n;
  }
  return n;
}

int recv_batch_cb(void* user, rudp_in_packet* packets, size_t max_count) {
  auto* ctx = static_cast<SockCtx*>(user);
  if (!ctx || ctx->fd < 0) return -1;
  ensure_physical_buffers(ctx);
  ctx->rx_pending_delivered.clear();
  size_t delivered = drain_pending_rx(ctx, packets, max_count);
  if (delivered >= max_count) return static_cast<int>(delivered);
  std::array<mmsghdr, IO_BATCH_MAX> msgs{};
  std::array<iovec, IO_BATCH_MAX> iov{};
  std::array<sockaddr_in, IO_BATCH_MAX> addrs{};
  size_t n_msgs = std::min(max_count - delivered, IO_BATCH_MAX);
  for (size_t i = 0; i < n_msgs; ++i) {
    iov[i].iov_base = ctx->rx_phys_data.data() + i * PHYSICAL_MTU;
    iov[i].iov_len = PHYSICAL_MTU;
    msgs[i].msg_hdr.msg_iov = &iov[i];
    msgs[i].msg_hdr.msg_iovlen = 1;
    msgs[i].msg_hdr.msg_name = &addrs[i];
    msgs[i].msg_hdr.msg_namelen = sizeof(addrs[i]);
  }
  int n = ::recvmmsg(ctx->fd, msgs.data(), static_cast<unsigned int>(n_msgs),
                     MSG_DONTWAIT, nullptr);
  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      return static_cast<int>(delivered);
    }
    return delivered > 0 ? static_cast<int>(delivered) : -1;
  }
  for (int i = 0; i < n; ++i) {
    rudp_addr src = to_rudp_addr(addrs[i]);
    const uint8_t* data = ctx->rx_phys_data.data() + (size_t)i * PHYSICAL_MTU;
    deliver_physical_packet(ctx, src, data, msgs[i].msg_len, packets, max_count,
                            &delivered);
  }
  return static_cast<int>(delivered);
}

uint64_t real_now_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          clock_type::now().time_since_epoch())
          .count());
}

uint64_t now_ns_cb(void* user) {
  auto* ctx = static_cast<SockCtx*>(user);
  if (ctx && ctx->cached_now_ns != 0) return ctx->cached_now_ns;
  return real_now_ns();
}

class CoopRudpAdapter : public rudp_bench::Adapter {
 public:
  ~CoopRudpAdapter() override { close_impl(); }

  void hint_connections(uint32_t n) override {
    hinted_connections_ = std::min(n, MAX_ADAPTER_CONNECTIONS);
  }

  void server_listen(uint16_t port) override {
    close_impl();
    is_server_ = true;
    ctx_.fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (ctx_.fd < 0) std::abort();
    int reuse = 1;
    ::setsockopt(ctx_.fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    tune_socket_buffers(ctx_.fd, "server");
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (::bind(ctx_.fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
      std::abort();
    }
    set_nonblock(ctx_.fd);
    create_endpoint();
    start_tx_worker();
    conn_by_id_.clear();
    conn_by_id_.reserve(std::max<uint32_t>(hinted_connections_, 64));
    connected_peak_ = 0;
    conn_cache_complete_ = false;
  }

  uint32_t client_connect(const char* host, uint16_t port) override {
    if (!host) return 0;
    sockaddr_in srv{};
    srv.sin_family = AF_INET;
    srv.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &srv.sin_addr) != 1) return 0;
    is_server_ = false;
    if (ctx_.fd < 0) {
      ctx_.fd = ::socket(AF_INET, SOCK_DGRAM, 0);
      if (ctx_.fd < 0) std::abort();
      tune_socket_buffers(ctx_.fd, "client");
      set_nonblock(ctx_.fd);
      create_endpoint();
    }
    uint32_t wire = next_wire_id();
    if (wire == 0) return 0;
    rudp_addr peer = to_rudp_addr(srv);
    rudp_conn* c = nullptr;
    if (rudp_endpoint_connect(ep_, &peer, wire, &c) != 0) return 0;
    remember_conn(wire, c);
    return wire;
  }

  bool is_connected(uint32_t conn_id) override {
    return ep_ && find_conn(conn_id) != nullptr;
  }

  // send()/send_many() reuse the timestamp cached at poll() entry. The
  // harness polls every tick, so staleness is bounded by one tick; message
  // enqueue_ns only feeds queue-delay stats and optional deadlines (unused
  // here), while RTO timing uses flush-time clocks.
  int send(uint32_t conn_id, const void* data, size_t len, bool reliable) override {
    if (!ep_) return -1;
    rudp_conn* c = find_conn(conn_id);
    if (!c) return -1;
    rudp_send_opts opts{};
    opts.reliability = reliable ? RUDP_RELIABLE_UNORDERED : RUDP_UNRELIABLE;
    rudp_send_result sr = rudp_send(c, data, len, &opts);
    if (sr != RUDP_SEND_QUEUED && sr != RUDP_SEND_OK) return -1;
    send_pending_ = true;
    return 0;
  }

  size_t send_many(const uint32_t* conn_ids, size_t count, const void* data,
                   size_t len, bool reliable) override {
    if (!ep_) return 0;
    if (!conn_ids && count > 0) return 0;
    const std::vector<rudp_conn*>* conns = cached_broadcast_conns(conn_ids, count);
    size_t accepted = 0;
    rudp_send_opts opts{};
    opts.reliability = reliable ? RUDP_RELIABLE_UNORDERED : RUDP_UNRELIABLE;
    for (size_t i = 0; i < count; ++i) {
      rudp_conn* c = conns ? (*conns)[i] : find_conn(conn_ids[i]);
      if (!c) continue;
      rudp_send_result sr = rudp_send(c, data, len, &opts);
      if (sr == RUDP_SEND_QUEUED || sr == RUDP_SEND_OK) ++accepted;
    }
    if (accepted > 0) send_pending_ = true;
    return accepted;
  }

  int recv(void* buf, size_t cap, size_t* out_len, uint32_t* out_conn_id) override {
    if (!ep_) return 0;
    if (!out_len || !out_conn_id || (!buf && cap != 0)) return -1;
    for (;;) {
      rudp_recv_info info{};
      int r = rudp_recv(ep_, buf, cap, out_len, &info);
      if (r != 1 && r != -1) return r;
      if (info.conn_id > std::numeric_limits<uint32_t>::max()) {
        if (r == -1) {
          const void* ignored_data = nullptr;
          size_t ignored_len = 0;
          rudp_recv_info ignored_info{};
          (void)rudp_recv_borrow(ep_, &ignored_data, &ignored_len,
                                  &ignored_info);
        }
        continue;
      }
      *out_conn_id = static_cast<uint32_t>(info.conn_id);
      if (info.conn) {
        remember_conn(*out_conn_id, info.conn);
      } else if (!conn_cache_complete_) {
        if (rudp_conn* c = rudp_endpoint_find_conn(ep_, info.conn_id)) {
          remember_conn(*out_conn_id, c);
        }
      }
      activity_since_poll_ = true;
      return r;
    }
  }

  void poll() override {
    if (!ep_) return;
    uint64_t now = real_now_ns();
    ctx_.cached_now_ns = now;
    uint64_t idle_poll_min_ns = coop_idle_poll_min_ns();
    if (!send_pending_ && !activity_since_poll_ && idle_poll_min_ns > 0 &&
        now < next_idle_poll_ns_) {
      return;
    }
    send_pending_ = false;
    activity_since_poll_ = false;
    rudp_endpoint_flush(ep_, now);
    rudp_endpoint_poll(ep_, now);
    next_idle_poll_ns_ =
        now > std::numeric_limits<uint64_t>::max() - idle_poll_min_ns
            ? std::numeric_limits<uint64_t>::max()
            : now + idle_poll_min_ns;
  }

  void close() override { close_impl(); }

 private:
  void close_impl() {
    stop_tx_worker();
    if (ep_) {
      rudp_endpoint_destroy(ep_);
      ep_ = nullptr;
    }
    if (ctx_.fd >= 0) {
      ::close(ctx_.fd);
      ctx_.fd = -1;
    }
    clear_io_state();
    connected_peak_ = 0;
    conn_cache_complete_ = false;
    conn_by_id_.clear();
    send_pending_ = false;
    activity_since_poll_ = false;
    next_idle_poll_ns_ = 0;
    broadcast_cache_ids_ = nullptr;
    broadcast_cache_count_ = 0;
    broadcast_cache_generation_ = 0;
    broadcast_conn_cache_.clear();
  }

 public:
  const char* name() const override { return "coop_rudp"; }
  bool supports(bool) const override { return true; }
  size_t max_payload_bytes(bool) const override {
    return MAX_ADAPTER_PAYLOAD_BYTES;
  }
  uint32_t max_connections() const override { return MAX_ADAPTER_CONNECTIONS; }
  const char* flush_policy(bool reliable) const override {
    return reliable ? "coop_flush_sack_retransmit" : "coop_flush_unreliable";
  }
  bool encryption_on() const override { return false; }
  // safe_bps の AIMD を pacing トークンとして送信ゲートに接続済み。
  // 初期レートは実質無制限（initial_safe_bps 既定 UINT32_MAX）。
  const char* congestion_control() const override { return "rate_aimd_paced"; }
  const char* thread_model() const override {
    return ctx_.async_tx ? "adapter_worker" : "single";
  }
  rudp_bench::ConnectionStats connection_stats() const override {
    rudp_bench::ConnectionStats s;
    s.connected_peak = connected_peak_;
    return s;
  }

 private:
  void start_tx_worker() {
    if (!is_server_ || !coop_async_tx_enabled() || ctx_.tx_thread.joinable()) return;
    ctx_.async_tx = true;
    ctx_.tx_stop = false;
    ctx_.tx_thread = std::thread([this]() { tx_loop(); });
  }

  void stop_tx_worker() {
    if (!ctx_.tx_thread.joinable()) {
      ctx_.async_tx = false;
      return;
    }
    {
      std::lock_guard<std::mutex> lock(ctx_.tx_mu);
      ctx_.tx_stop = true;
      ctx_.tx_queue.clear();
      ctx_.tx_queue_bytes = 0;
    }
    ctx_.tx_cv.notify_one();
    ctx_.tx_thread.join();
    ctx_.async_tx = false;
    ctx_.tx_stop = false;
  }

  void clear_io_state() {
    {
      std::lock_guard<std::mutex> lock(ctx_.tx_mu);
      ctx_.tx_queue.clear();
      ctx_.tx_free.clear();
      ctx_.tx_queue_bytes = 0;
      ctx_.tx_stop = false;
    }
    ctx_.rx_pending.clear();
    ctx_.rx_pending_delivered.clear();
    ctx_.rx_pending_bytes = 0;
    ctx_.tx_phys_data.clear();
    ctx_.rx_phys_data.clear();
  }

  uint32_t next_wire_id() {
    if (conn_by_id_.size() >= MAX_ADAPTER_CONNECTIONS) return 0;
    for (uint32_t attempts = 0; attempts < MAX_ADAPTER_CONNECTIONS * 2u; ++attempts) {
      uint32_t local = next_local_id_++;
      if (local == 0) local = next_local_id_++;
      uint32_t wire = wire_prefix_ ^ local;
      if (wire != 0 && conn_by_id_.find(wire) == conn_by_id_.end()) {
        return wire;
      }
    }
    return 0;
  }

  void tx_loop() {
    std::vector<AsyncTxPacket> batch;
    batch.reserve(IO_BATCH_MAX);
    std::array<rudp_out_packet, IO_BATCH_MAX> out{};
    while (true) {
      batch.clear();
      {
        std::unique_lock<std::mutex> lock(ctx_.tx_mu);
        ctx_.tx_cv.wait(lock, [this]() {
          return ctx_.tx_stop || !ctx_.tx_queue.empty();
        });
        if (ctx_.tx_stop) break;
        while (!ctx_.tx_queue.empty() && batch.size() < IO_BATCH_MAX) {
          size_t bytes = ctx_.tx_queue.front().bytes.size();
          batch.push_back(std::move(ctx_.tx_queue.front()));
          ctx_.tx_queue.pop_front();
          if (ctx_.tx_queue_bytes >= bytes) {
            ctx_.tx_queue_bytes -= bytes;
          } else {
            ctx_.tx_queue_bytes = 0;
          }
        }
      }
      for (size_t i = 0; i < batch.size(); ++i) {
        out[i].addr = batch[i].addr;
        out[i].data = batch[i].bytes.data();
        out[i].len = batch[i].bytes.size();
      }
      (void)send_packets_batched_now(&ctx_, out.data(), batch.size());
      {
        // 送り終わったバッファをプールへ返す（上限付き）。
        constexpr size_t kTxFreePoolMax = 4096;
        std::lock_guard<std::mutex> lock(ctx_.tx_mu);
        for (auto& p : batch) {
          if (ctx_.tx_free.size() >= kTxFreePoolMax) break;
          ctx_.tx_free.push_back(std::move(p.bytes));
        }
      }
    }
  }

  rudp_conn* find_conn(uint32_t conn_id) {
    auto it = conn_by_id_.find(conn_id);
    if (it != conn_by_id_.end()) {
      // usable だけでなく conn_id も照合する。コア側でスロットが別の
      // コネクションに再利用されていた場合、生ポインタは「usable な別人」を
      // 指すことがあり、照合しないと誤送信になる。
      if (conn_is_usable(it->second) &&
          rudp_conn_id(it->second) == conn_id) {
        return it->second;
      }
      conn_by_id_.erase(it);
      ++conn_cache_generation_;
      conn_cache_complete_ = false;
      return nullptr;
    }
    rudp_conn* c = rudp_endpoint_find_conn(ep_, conn_id);
    if (c) remember_conn(conn_id, c);
    return c;
  }

  bool conn_is_usable(rudp_conn* conn) const {
    rudp_status st{};
    rudp_get_status(conn, &st);
    return st.usable != 0;
  }

  void remember_conn(uint32_t conn_id, rudp_conn* conn) {
    if (!conn || !conn_is_usable(conn)) return;
    auto [it, inserted] = conn_by_id_.try_emplace(conn_id, conn);
    if (!inserted && it->second == conn) {
      refresh_conn_cache_complete();
      return;
    }
    if (!inserted) it->second = conn;
    ++conn_cache_generation_;
    connected_peak_ =
        std::max<uint32_t>(connected_peak_, static_cast<uint32_t>(conn_by_id_.size()));
    refresh_conn_cache_complete();
  }

  void refresh_conn_cache_complete() {
    if (hinted_connections_ > 0 && conn_by_id_.size() >= hinted_connections_) {
      conn_cache_complete_ = true;
    }
  }

  void create_endpoint() {
    rudp_endpoint_config cfg{};
    cfg.socket.user = &ctx_;
    cfg.socket.send_batch = send_batch_cb;
    cfg.socket.recv_batch = recv_batch_cb;
    cfg.socket.now_ns = now_ns_cb;
    cfg.mtu = DEFAULT_MTU;
    if (hinted_connections_ > 0) {
      uint32_t headroom = std::max<uint32_t>(1, hinted_connections_ / 16);
      cfg.max_conns =
          hinted_connections_ > MAX_ADAPTER_CONNECTIONS - headroom
              ? MAX_ADAPTER_CONNECTIONS
              : hinted_connections_ + headroom;
    } else {
      cfg.max_conns = 64;
    }
    cfg.max_flows = 1;
    cfg.max_channels = 1;
    cfg.max_messages =
        std::max<uint32_t>(4096, cfg.max_conns * MESSAGES_PER_CONNECTION);
    bool broad_recv = cfg.max_conns <= 512;
    if (is_server_ && broad_recv) {
      // Broadcast rooms fan every inbound message out to all peers, and the
      // 1Hz reliable events arrive near-synchronized across clients, so up to
      // conns^2 reliable messages stay allocated for ~RTT until acked. Size
      // the pool for one full fanout burst or rudp_send rejects ~half of it.
      // 1.5x headroom: unreliable fanout keeps allocating while the burst
      // drains, so sizing exactly one burst still rejects the tail of it.
      uint64_t fanout_burst = (uint64_t)cfg.max_conns * cfg.max_conns * 3u / 2u +
                              (uint64_t)cfg.max_conns * MESSAGES_PER_CONNECTION;
      cfg.max_messages = std::max<uint32_t>(
          cfg.max_messages,
          (uint32_t)std::min<uint64_t>(fanout_burst, 196608));
    }
    cfg.max_recv_events =
        broad_recv
            ? std::max<uint32_t>(32768, cfg.max_conns * BROAD_RECV_EVENTS_PER_CONNECTION)
            : std::max<uint32_t>(8192, cfg.max_conns * NARROW_RECV_EVENTS_PER_CONNECTION);
    cfg.max_ordered_holds = 1024;
    cfg.sent_packet_count = 512;
    cfg.recv_batch_size =
        std::min<uint32_t>(cfg.max_recv_events, broad_recv ? 32768 : 4096);
    cfg.send_batch_size = 256;
    cfg.rto_ms = 100;  // RTT サンプル取得前の初期 RTO。以後は適応 RTO。
    cfg.min_rto_ms = u32_env_or_default("COOP_MIN_RTO_MS", 0);  // 0 = core 既定
    cfg.max_rto_ms = u32_env_or_default("COOP_MAX_RTO_MS", 0);  // 0 = core 既定
    cfg.max_retransmits =
        u32_env_or_default("COOP_MAX_RETRANSMITS", DEFAULT_MAX_RETRANSMITS);
    cfg.idle_timeout_ms =
        u32_env_or_default("COOP_IDLE_TIMEOUT_MS", DEFAULT_IDLE_TIMEOUT_MS);
    // adapter は単一フレーム運用（多フラグメント送信をしない）ので再組立
    // スロットは最小限でよい。既定の max_recv_events と同数だと ~37MB が
    // 死蔵される。
    cfg.max_reassemblies = 256;
    // ベンチではハンドシェイクなしの高速 ramp が必要なので既定は無制限。
    cfg.max_incoming_conns_per_poll =
        u32_env_or_default("COOP_INCOMING_CONN_LIMIT", 0);
    cfg.skip_unreliable_acks = 1;
    if (rudp_endpoint_create(&ep_, &cfg) != 0) std::abort();
    ensure_physical_buffers(&ctx_);
  }

  const std::vector<rudp_conn*>* cached_broadcast_conns(const uint32_t* conn_ids,
                                                        size_t count) {
    if (conn_ids == broadcast_cache_ids_ && count == broadcast_cache_count_ &&
        broadcast_cache_generation_ == conn_cache_generation_) {
      return &broadcast_conn_cache_;
    }
    broadcast_conn_cache_.clear();
    broadcast_conn_cache_.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      broadcast_conn_cache_.push_back(find_conn(conn_ids[i]));
    }
    broadcast_cache_ids_ = conn_ids;
    broadcast_cache_count_ = count;
    broadcast_cache_generation_ = conn_cache_generation_;
    return &broadcast_conn_cache_;
  }

  SockCtx ctx_;
  rudp_endpoint* ep_ = nullptr;
  bool is_server_ = false;
  uint32_t hinted_connections_ = 0;
  uint32_t next_local_id_ = 1;
  std::unordered_map<uint32_t, rudp_conn*> conn_by_id_;
  uint64_t conn_cache_generation_ = 0;
  bool send_pending_ = false;
  bool activity_since_poll_ = false;
  uint64_t next_idle_poll_ns_ = 0;
  const uint32_t* broadcast_cache_ids_ = nullptr;
  size_t broadcast_cache_count_ = 0;
  uint64_t broadcast_cache_generation_ = 0;
  std::vector<rudp_conn*> broadcast_conn_cache_;
  uint32_t connected_peak_ = 0;
  bool conn_cache_complete_ = false;
  uint32_t wire_prefix_ =
      (static_cast<uint32_t>(::getpid()) & 0xffffu) << 16;
};

}  // namespace

namespace rudp_bench {
void register_coop_rudp_adapter() {
  register_adapter("coop_rudp", []() { return std::make_unique<CoopRudpAdapter>(); });
}
}  // namespace rudp_bench
