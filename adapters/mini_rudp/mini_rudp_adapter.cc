#include "harness/adapter.h"
#include "harness/adapter_registry.h"
#include "harness/inbound_queue.h"
#include "harness/sliding_dedup_window.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

constexpr uint16_t FLAG_ACK = 1;
constexpr uint16_t FLAG_REL = 2;
constexpr size_t MAX_UDP_PAYLOAD = 65507;

struct Header {
  uint16_t flags;
  uint32_t seq;
} __attribute__((packed));

struct PendingSend {
  std::vector<uint8_t> bytes;
  std::chrono::steady_clock::time_point sent_at;
};

struct Conn {
  int fd = -1;
  sockaddr_in peer{};
  uint32_t next_seq = 1;
  std::unordered_map<uint32_t, PendingSend> pending;     // 未 ACK
  rudp_bench::SlidingDedupWindow received_seq;           // bounded duplicate suppression
};

struct PayloadView {
  const uint8_t* data = nullptr;
  size_t len = 0;
};

uint64_t addr_key(const sockaddr_in& a) {
  return (static_cast<uint64_t>(a.sin_addr.s_addr) << 32) |
         static_cast<uint64_t>(a.sin_port);
}

void set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

class MiniRudpAdapter : public rudp_bench::Adapter {
 public:
  void server_listen(uint16_t port) override {
    server_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    int reuse = 1;
    ::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    ::bind(server_fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    set_nonblock(server_fd_);
  }

  uint32_t client_connect(const char* host, uint16_t port) override {
    Conn c;
    c.fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    set_nonblock(c.fd);
    c.peer.sin_family = AF_INET;
    c.peer.sin_port = htons(port);
    inet_pton(AF_INET, host, &c.peer.sin_addr);
    uint32_t id = next_id_++;
    conns_[id] = std::move(c);
    pollfds_.push_back(pollfd{conns_[id].fd, POLLIN, 0});
    poll_conn_ids_.push_back(id);
    return id;
  }

  bool is_connected(uint32_t) override { return true; }

  int send(uint32_t conn_id, const void* data, size_t len, bool reliable) override {
    if (len > max_payload_bytes(reliable)) return -1;
    auto* c = find_conn(conn_id);
    if (!c) return -1;
    Header h;
    h.flags = reliable ? FLAG_REL : 0;
    h.seq = c->next_seq++;
    int sent_fd = (server_fd_ >= 0) ? server_fd_ : c->fd;
    sockaddr_in& peer = c->peer;

    std::vector<uint8_t> pkt;
    std::vector<uint8_t>* out = &send_scratch_;
    if (reliable) {
      pkt.resize(sizeof(h) + len);
      out = &pkt;
    } else {
      send_scratch_.resize(sizeof(h) + len);
    }
    std::memcpy(out->data(), &h, sizeof(h));
    std::memcpy(out->data() + sizeof(h), data, len);
    ssize_t n = ::sendto(sent_fd, out->data(), out->size(), 0,
                        reinterpret_cast<sockaddr*>(&peer), sizeof(peer));
    if (n != static_cast<ssize_t>(out->size())) return -1;
    if (reliable) {
      c->pending[h.seq] = {std::move(pkt), std::chrono::steady_clock::now()};
    }
    return 0;
  }

  int recv(void* buf, size_t cap, size_t* out_len, uint32_t* out_conn_id) override {
    sockaddr_in src{};
    socklen_t sl = sizeof(src);
    int fd = (server_fd_ >= 0) ? server_fd_ : -1;
    if (fd < 0) {
      return inbox_.recv(buf, cap, out_len, out_conn_id);
    }
    // server: single fd
    uint8_t pkt[MAX_UDP_PAYLOAD];
    ssize_t n = ::recvfrom(fd, pkt, sizeof(pkt), 0,
                          reinterpret_cast<sockaddr*>(&src), &sl);
    if (n <= 0) return 0;
    uint64_t k = addr_key(src);
    auto it = id_by_key_.find(k);
    uint32_t id;
    if (it == id_by_key_.end()) {
      id = next_id_++;
      id_by_key_[k] = id;
      Conn c;
      c.peer = src;
      conns_[id] = std::move(c);
    } else {
      id = it->second;
    }
    PayloadView payload;
    if (handle_packet(conns_[id], pkt, n, &payload)) {
      if (payload.len > cap) return 0;
      if (payload.len > 0) std::memcpy(buf, payload.data, payload.len);
      *out_len = payload.len;
      *out_conn_id = id;
      return 1;
    }
    return 0;
  }

  void poll() override {
    poll_client_sockets();

    auto now = std::chrono::steady_clock::now();
    for (auto& [id, c] : conns_) {
      for (auto& [seq, ps] : c.pending) {
        if (now - ps.sent_at > std::chrono::milliseconds(50)) {
          int fd = (server_fd_ >= 0) ? server_fd_ : c.fd;
          ::sendto(fd, ps.bytes.data(), ps.bytes.size(), 0,
                  reinterpret_cast<sockaddr*>(&c.peer), sizeof(c.peer));
          ps.sent_at = now;
        }
      }
    }
  }

  void close() override {
    if (server_fd_ >= 0) { ::close(server_fd_); server_fd_ = -1; }
    for (auto& [id, c] : conns_) if (c.fd >= 0) ::close(c.fd);
    conns_.clear();
    pollfds_.clear();
    poll_conn_ids_.clear();
    inbox_.clear();
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

  void poll_client_sockets() {
    if (server_fd_ >= 0 || pollfds_.empty()) return;
    int ready = ::poll(pollfds_.data(), static_cast<nfds_t>(pollfds_.size()), 0);
    if (ready <= 0) return;

    uint8_t pkt[MAX_UDP_PAYLOAD];
    for (size_t i = 0; i < pollfds_.size() && ready > 0; ++i) {
      if ((pollfds_[i].revents & POLLIN) == 0) continue;
      --ready;
      Conn* c = find_conn(poll_conn_ids_[i]);
      if (!c) continue;
      for (;;) {
        ssize_t n = ::recv(pollfds_[i].fd, pkt, sizeof(pkt), 0);
        if (n <= 0) break;
        PayloadView payload;
        if (handle_packet(*c, pkt, n, &payload)) {
          inbox_.enqueue(poll_conn_ids_[i], payload.data, payload.len);
        }
      }
    }
  }

  bool handle_packet(Conn& c, const uint8_t* pkt, ssize_t n,
                     PayloadView* payload_out) {
    if (n < static_cast<ssize_t>(sizeof(Header))) return false;
    Header h;
    std::memcpy(&h, pkt, sizeof(h));
    size_t payload = static_cast<size_t>(n) - sizeof(h);
    if (h.flags & FLAG_ACK) {
      c.pending.erase(h.seq);
      return false;
    }
    if (h.flags & FLAG_REL) {
      Header ack{};
      ack.flags = FLAG_ACK;
      ack.seq = h.seq;
      int fd = (server_fd_ >= 0) ? server_fd_ : c.fd;
      ::sendto(fd, &ack, sizeof(ack), 0,
              reinterpret_cast<sockaddr*>(&c.peer), sizeof(c.peer));
      if (!c.received_seq.insert(h.seq)) return false;
    }
    payload_out->data = pkt + sizeof(h);
    payload_out->len = payload;
    return true;
  }

  int server_fd_ = -1;
  std::unordered_map<uint32_t, Conn> conns_;
  std::unordered_map<uint64_t, uint32_t> id_by_key_;
  std::vector<pollfd> pollfds_;
  std::vector<uint32_t> poll_conn_ids_;
  std::vector<uint8_t> send_scratch_;
  rudp_bench::ReusableInboundQueue inbox_;
  uint32_t next_id_ = 1;
};

}  // namespace

namespace rudp_bench {
void register_mini_rudp_adapter() {
  register_adapter("mini_rudp",
      []() { return std::make_unique<MiniRudpAdapter>(); });
}
}  // namespace rudp_bench
