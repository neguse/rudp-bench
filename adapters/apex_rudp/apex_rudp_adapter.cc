#include "harness/adapter.h"
#include "harness/adapter_registry.h"
#include "harness/inbound_queue.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <deque>
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
// ============================================================

namespace {

using clock_type = std::chrono::steady_clock;

constexpr uint8_t FLAG_REL = 0x01;
constexpr uint8_t FLAG_ACK_ONLY = 0x02;
constexpr uint8_t FLAG_HAS_ACK = 0x04;

constexpr size_t MAX_UDP_PAYLOAD = 65507;
constexpr size_t HEADER_BYTES = 1 + 4 + 4 + 4 + 8;  // flags, conv, seq, ack, ack_bits
constexpr size_t MAX_PAYLOAD = MAX_UDP_PAYLOAD - HEADER_BYTES;
constexpr size_t MAX_PENDING_RELIABLE_PER_CONN = 4096;
constexpr size_t INBOX_LIMIT = 1u << 20;
constexpr int DEFAULT_SOCKET_BUFFER_KB = 4096;
constexpr auto RETX_TIMEOUT = std::chrono::milliseconds(100);
constexpr int DEFAULT_ACK_DELAY_MS = 2;
constexpr size_t RECV_BATCH = 64;
constexpr size_t TX_BATCH = 64;
constexpr size_t MAX_DRAIN_PACKETS = 4096;
constexpr size_t MAX_TX_QUEUE = 1u << 20;
constexpr size_t MAX_RX_QUEUE = 1u << 20;

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

uint64_t load_u64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
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

void set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void tune_socket_buffers(int fd) {
  int kb = DEFAULT_SOCKET_BUFFER_KB;
  if (const char* v = std::getenv("APEX_RCVBUF_KB"); v && *v) {
    int parsed = std::atoi(v);
    if (parsed > 0) kb = parsed;
  }
  int bytes = kb * 1024;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes));
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

class RecvSackWindow {
 public:
  bool insert(uint32_t seq) {
    if (seq == 0) return false;
    if (highest_ == 0) {
      highest_ = seq;
      bits_ = 1;
      return true;
    }
    if (seq > highest_) {
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
  clock_type::time_point sent_at{};
};

struct Conn {
  sockaddr_in peer{};
  uint32_t conv = 0;
  uint32_t next_seq = 1;
  bool active = false;
  bool ack_dirty = false;
  clock_type::time_point ack_due = clock_type::time_point::max();
  RecvSackWindow recv_window;
  std::deque<PendingSend> pending;
};

struct Peer {
  std::vector<uint32_t> id_by_conv;
};

struct TxDatagram {
  sockaddr_in peer{};
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

bool ack_covers(uint32_t ack, uint64_t ack_bits, uint32_t seq) {
  if (ack == 0 || seq > ack) return false;
  uint32_t delta = ack - seq;
  if (delta >= 64) return false;
  return (ack_bits & (uint64_t{1} << delta)) != 0;
}

class ApexRudpAdapter : public rudp_bench::Adapter {
 public:
  ApexRudpAdapter() {
    inbox_.set_limit(INBOX_LIMIT);
    direct_batch_.reserve(TX_BATCH);
  }
  ~ApexRudpAdapter() override { close(); }

  void hint_connections(uint32_t n) override {
    conns_.reserve(n);
    peers_.reserve(8);
  }

  void server_listen(uint16_t port) override {
    is_server_ = true;
    server_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (server_fd_ < 0) std::abort();
    int reuse = 1;
    ::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    tune_socket_buffers(server_fd_);
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
      tune_socket_buffers(server_ack_fd_);
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
    if (client_fd_ < 0) {
      client_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
      if (client_fd_ < 0) std::abort();
      tune_socket_buffers(client_fd_);
      set_nonblock(client_fd_);
      sockaddr_in srv{};
      srv.sin_family = AF_INET;
      srv.sin_port = htons(port);
      if (inet_pton(AF_INET, host, &srv.sin_addr) != 1) std::abort();
      server_peer_ = srv;
      server_ack_peer_ = srv;
      server_ack_peer_.sin_port = htons(static_cast<uint16_t>(port + 1));
      if (async_send_enabled_for(false)) start_tx_worker();
    }

    uint32_t id = next_id_++;
    Conn c;
    c.active = true;
    c.conv = id;
    c.peer = server_peer_;
    conns_.push_back(std::move(c));
    connected_peak_ = std::max<uint32_t>(connected_peak_, conns_.size());
    return id;
  }

  bool is_connected(uint32_t conn_id) override { return find_conn(conn_id) != nullptr; }

  int send(uint32_t conn_id, const void* data, size_t len, bool reliable) override {
    if (len > max_payload_bytes(reliable)) return -1;
    Conn* c = find_conn(conn_id);
    if (!c) return -1;
    if (reliable && c->pending.size() >= MAX_PENDING_RELIABLE_PER_CONN) return -1;

    uint32_t seq = reliable ? c->next_seq++ : 0;
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
      c->pending.push_back(PendingSend{seq, std::move(pkt), now});
      PendingSend& pending = c->pending.back();
      if (!queue_send_ref(*c, pending.bytes.data(), pending.bytes.size())) {
        recycle_buffer(std::move(pending.bytes));
        c->pending.pop_back();
        return -1;
      }
      note_ack_sent(*c, h);
      schedule_retx(now + RETX_TIMEOUT);
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

  int recv(void* buf, size_t cap, size_t* out_len, uint32_t* out_conn_id) override {
    if (recv_drain_on_empty_enabled() && inbox_.empty() &&
        recv_empty_drains_left_ > 0) {
      flush_direct_batch();
      drain_socket(MAX_DRAIN_PACKETS / 4);
      --recv_empty_drains_left_;
    }
    return inbox_.recv(buf, cap, out_len, out_conn_id);
  }

  void poll() override {
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
    if (client_fd_ >= 0) {
      ::close(client_fd_);
      client_fd_ = -1;
    }
    if (server_ack_fd_ >= 0) {
      ::close(server_ack_fd_);
      server_ack_fd_ = -1;
    }
    conns_.clear();
    peers_.clear();
    free_buffers_.clear();
    inbox_.clear();
    next_id_ = 1;
    connected_peak_ = 0;
    earliest_ack_ = clock_type::time_point::max();
    earliest_retx_ = clock_type::time_point::max();
  }

  const char* name() const override { return "apex_rudp"; }
  bool supports(bool) const override { return true; }
  size_t max_payload_bytes(bool /*reliable*/) const override { return MAX_PAYLOAD; }
  const char* flush_policy(bool reliable) const override {
    return reliable ? "piggyback_sack_retransmit"
                    : "server_async_unreliable_piggyback_ack";
  }
  bool encryption_on() const override { return false; }
  rudp_bench::ConnectionStats connection_stats() const override {
    rudp_bench::ConnectionStats s;
    s.connected_peak = connected_peak_;
    return s;
  }

 private:
  Conn* find_conn(uint32_t id) {
    if (id == 0 || id > conns_.size()) return nullptr;
    Conn& c = conns_[id - 1];
    return c.active ? &c : nullptr;
  }

  Conn* server_conn_for(const sockaddr_in& src, uint32_t conv, bool create) {
    uint64_t key = addr_key(src);
    auto peer_it = peers_.find(key);
    if (peer_it != peers_.end()) {
      Peer& peer = peer_it->second;
      if (conv < peer.id_by_conv.size()) {
        uint32_t existing = peer.id_by_conv[conv];
        if (existing != 0) return find_conn(existing);
      }
    }
    if (!create) return nullptr;

    Peer& peer = peers_[key];
    if (conv >= peer.id_by_conv.size()) {
      peer.id_by_conv.resize(static_cast<size_t>(conv) + 1, 0);
    }

    uint32_t id = next_id_++;
    peer.id_by_conv[conv] = id;
    Conn c;
    c.active = true;
    c.conv = conv;
    c.peer = src;
    conns_.push_back(std::move(c));
    connected_peak_ = std::max<uint32_t>(connected_peak_, conns_.size());
    return &conns_.back();
  }

  bool queue_send_ref(const Conn& c, const uint8_t* data, size_t len) {
    if ((is_server_ ? server_fd_ : client_fd_) < 0) return false;
    if (!async_send_enabled_for(is_server_)) {
      if (direct_batch_.size() >= MAX_TX_QUEUE) return false;
      TxDatagram d;
      d.peer = c.peer;
      d.external = data;
      d.len = len;
      direct_batch_.push_back(std::move(d));
      if (direct_batch_.size() >= TX_BATCH) flush_direct_batch();
      return true;
    }

    TxDatagram d;
    d.peer = c.peer;
    d.owned.resize(len);
    std::memcpy(d.owned.data(), data, len);
    {
      std::lock_guard<std::mutex> lock(tx_mu_);
      if (tx_queue_.size() >= MAX_TX_QUEUE) return false;
      tx_queue_.push_back(std::move(d));
    }
    tx_cv_.notify_one();
    return true;
  }

  bool queue_send_owned(const Conn& c, std::vector<uint8_t>&& bytes) {
    if ((is_server_ ? server_fd_ : client_fd_) < 0) return false;
    if (!async_send_enabled_for(is_server_)) {
      if (direct_batch_.size() >= MAX_TX_QUEUE) return false;
      TxDatagram d;
      d.peer = c.peer;
      d.owned = std::move(bytes);
      direct_batch_.push_back(std::move(d));
      if (direct_batch_.size() >= TX_BATCH) flush_direct_batch();
      return true;
    }

    TxDatagram d;
    d.peer = c.peer;
    d.owned = std::move(bytes);
    {
      std::lock_guard<std::mutex> lock(tx_mu_);
      if (tx_queue_.size() >= MAX_TX_QUEUE) return false;
      tx_queue_.push_back(std::move(d));
    }
    tx_cv_.notify_one();
    return true;
  }

  bool queue_send_owned_async(const Conn& c, std::vector<uint8_t>&& bytes) {
    if ((is_server_ ? server_fd_ : client_fd_) < 0) return false;
    {
      std::lock_guard<std::mutex> lock(tx_mu_);
      if (tx_queue_.size() >= MAX_TX_QUEUE) return false;
      TxDatagram d;
      d.peer = c.peer;
      d.owned = std::move(bytes);
      tx_queue_.push_back(std::move(d));
    }
    tx_cv_.notify_one();
    return true;
  }

  bool queue_send_copy(const Conn& c, const uint8_t* data, size_t len) {
    return queue_send_copy_to(c.peer, data, len);
  }

  bool queue_send_copy_to(const sockaddr_in& peer, const uint8_t* data, size_t len) {
    std::vector<uint8_t> bytes = take_buffer();
    bytes.resize(len);
    std::memcpy(bytes.data(), data, len);
    Conn c;
    c.peer = peer;
    if (queue_send_owned(c, std::move(bytes))) return true;
    recycle_buffer(std::move(bytes));
    return false;
  }

  void start_tx_worker() {
    if (tx_worker_.joinable()) return;
    tx_stop_ = false;
    tx_worker_ = std::thread([this]() { tx_loop(); });
  }

  void stop_tx_worker() {
    {
      std::lock_guard<std::mutex> lock(tx_mu_);
      tx_stop_ = true;
      tx_queue_.clear();
    }
    tx_cv_.notify_one();
    if (tx_worker_.joinable()) tx_worker_.join();
  }

  void tx_loop() {
    std::vector<TxDatagram> batch;
    batch.reserve(TX_BATCH);
    while (true) {
      {
        std::unique_lock<std::mutex> lock(tx_mu_);
        tx_cv_.wait(lock, [&]() { return tx_stop_ || !tx_queue_.empty(); });
        if (tx_stop_) return;
        while (!tx_queue_.empty() && batch.size() < TX_BATCH) {
          batch.push_back(std::move(tx_queue_.front()));
          tx_queue_.pop_front();
        }
      }
      send_batch(batch);
      batch.clear();
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
    int fd = is_server_ ? server_fd_ : client_fd_;
    if (fd < 0 || batch.empty()) return 0;
    std::array<mmsghdr, TX_BATCH> msgs{};
    std::array<iovec, TX_BATCH> iov{};
    size_t n_msgs = std::min(batch.size(), TX_BATCH);
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

  void mark_ack_dirty(Conn& c, clock_type::time_point now) {
    if (!c.ack_dirty) {
      c.ack_dirty = true;
      c.ack_due = now + ack_delay();
    }
    if (c.ack_due < earliest_ack_) earliest_ack_ = c.ack_due;
  }

  void process_ack(Conn& c, uint32_t ack, uint64_t ack_bits) {
    if (ack == 0 || c.pending.empty()) return;
    for (auto it = c.pending.begin(); it != c.pending.end();) {
      if (it->seq > ack) break;
      if (ack_covers(ack, ack_bits, it->seq)) {
        recycle_buffer(std::move(it->bytes));
        it = c.pending.erase(it);
      } else {
        ++it;
      }
    }
  }

  void process_packet(const uint8_t* pkt, size_t len, const sockaddr_in& src,
                      clock_type::time_point now) {
    if (len < HEADER_BYTES) return;
    PacketHeader h = parse_header(pkt);

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
      process_ack(*c, h.ack, h.ack_bits);
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

  void drain_socket(size_t max_packets) {
    int fd = is_server_ ? server_fd_ : client_fd_;
    if (fd < 0 || max_packets == 0) return;
    if (rx_worker_.joinable()) {
      drain_rx_queue(max_packets);
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

  void service_retransmits(clock_type::time_point now) {
    auto next_due = clock_type::time_point::max();
    for (Conn& c : conns_) {
      if (!c.active) continue;
      for (PendingSend& p : c.pending) {
        auto due = p.sent_at + RETX_TIMEOUT;
        if (now >= due) {
          PacketHeader h = parse_header(p.bytes.data());
          h.flags = FLAG_REL;
          attach_ack(c, &h, true);
          write_header(p.bytes.data(), h);
          if (queue_send_ref(c, p.bytes.data(), p.bytes.size())) {
            note_ack_sent(c, h);
            p.sent_at = now;
          }
          due = p.sent_at + RETX_TIMEOUT;
        }
        if (due < next_due) next_due = due;
      }
    }
    earliest_retx_ = next_due;
  }

  void service_standalone_acks(clock_type::time_point now) {
    auto next_due = clock_type::time_point::max();
    std::array<uint8_t, HEADER_BYTES> pkt{};
    for (Conn& c : conns_) {
      if (!c.active || !c.ack_dirty) continue;
      if (now < c.ack_due) {
        if (c.ack_due < next_due) next_due = c.ack_due;
        continue;
      }
      PacketHeader h;
      h.flags = FLAG_ACK_ONLY;
      h.conv = c.conv;
      attach_ack(c, &h, true);
      if ((h.flags & FLAG_HAS_ACK) == 0) continue;
      write_header(pkt.data(), h);
      const sockaddr_in& ack_peer =
          (!is_server_ && split_ack_enabled()) ? server_ack_peer_ : c.peer;
      if (queue_send_copy_to(ack_peer, pkt.data(), pkt.size())) {
        note_ack_sent(c, h);
      } else {
        c.ack_due = now + ack_delay();
        if (c.ack_due < next_due) next_due = c.ack_due;
      }
    }
    earliest_ack_ = next_due;
  }

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

  void schedule_retx(clock_type::time_point due) {
    if (due < earliest_retx_) earliest_retx_ = due;
  }

  int server_fd_ = -1;
  int server_ack_fd_ = -1;
  int client_fd_ = -1;
  bool is_server_ = false;
  sockaddr_in server_peer_{};
  sockaddr_in server_ack_peer_{};
  std::vector<Conn> conns_;
  std::unordered_map<uint64_t, Peer> peers_;
  rudp_bench::ReusableInboundQueue inbox_;
  std::vector<std::vector<uint8_t>> free_buffers_;
  std::vector<TxDatagram> direct_batch_;
  std::mutex tx_mu_;
  std::condition_variable tx_cv_;
  std::deque<TxDatagram> tx_queue_;
  std::thread tx_worker_;
  bool tx_stop_ = false;
  std::mutex rx_mu_;
  std::deque<RxDatagram> rx_queue_;
  std::vector<std::vector<uint8_t>> rx_free_;
  std::thread rx_worker_;
  std::atomic<bool> rx_stop_{false};
  uint32_t next_id_ = 1;
  uint32_t connected_peak_ = 0;
  clock_type::time_point earliest_ack_ = clock_type::time_point::max();
  clock_type::time_point earliest_retx_ = clock_type::time_point::max();
  size_t recv_empty_drains_left_ = 0;

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
