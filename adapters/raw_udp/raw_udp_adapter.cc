#include "harness/adapter.h"
#include "harness/adapter_registry.h"
#include "harness/inbound_queue.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

constexpr size_t MAX_UDP_PAYLOAD = 65507;
constexpr int UDP_SOCKET_BUFFER_BYTES = 256 * 1024;
constexpr ssize_t RECV_WOULD_BLOCK = -2;

struct Conn {
  int fd = -1;
  sockaddr_in peer{};
};

uint64_t addr_key(const sockaddr_in& a) {
  return (static_cast<uint64_t>(a.sin_addr.s_addr) << 32) | static_cast<uint64_t>(a.sin_port);
}

[[noreturn]] void die_errno(const char* what) {
  std::perror(what);
  std::abort();
}

[[noreturn]] void die_msg(const char* what) {
  std::fprintf(stderr, "%s\n", what);
  std::abort();
}

void set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) die_errno("fcntl(F_GETFL)");
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) die_errno("fcntl(F_SETFL)");
}

void tune_socket_buffers(int fd) {
  int bytes = UDP_SOCKET_BUFFER_BYTES;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes));
}

int make_udp_socket() {
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) die_errno("socket(AF_INET, SOCK_DGRAM)");
  tune_socket_buffers(fd);
  set_nonblock(fd);
  return fd;
}

ssize_t recv_datagram(int fd, void* buf, size_t cap, sockaddr_in* src) {
  for (;;) {
    socklen_t sl = sizeof(*src);
    ssize_t n = ::recvfrom(fd, buf, cap, MSG_TRUNC,
                           reinterpret_cast<sockaddr*>(src), &sl);
    if (n >= 0) return n;
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return RECV_WOULD_BLOCK;
    return -1;
  }
}

ssize_t recv_client_datagram(int fd, void* buf, size_t cap) {
  for (;;) {
    ssize_t n = ::recv(fd, buf, cap, 0);
    if (n >= 0) return n;
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
    return -1;
  }
}

int send_datagram(int fd, const sockaddr_in& peer, const void* data, size_t len) {
  for (;;) {
    ssize_t n = ::sendto(fd, data, len, 0,
                         reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
    if (n == static_cast<ssize_t>(len)) return 0;
    if (n < 0 && errno == EINTR) continue;
    return -1;
  }
}

class RawUdpAdapter : public rudp_bench::Adapter {
 public:
  void server_listen(uint16_t port) override {
    server_fd_ = make_udp_socket();
    int reuse = 1;
    if (::setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
      die_errno("setsockopt(SO_REUSEADDR)");
    }
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
      die_errno("bind(raw_udp)");
    }
  }

  uint32_t client_connect(const char* host, uint16_t port) override {
    // L16: one fd per conn is intentional for this pure-UDP baseline. Unlike
    // mini_rudp (which multiplexes to survive at scale, L11), raw_udp is
    // unreliable-only with no per-conn protocol state, so the extra poll/recv
    // syscalls are light and the simpler per-fd model is the more honest
    // baseline. Reviewed: acceptable, kept as-is. Socket buffers are 256KB
    // (make_udp_socket -> tune_socket_buffers), matching the other adapters.
    Conn c;
    c.fd = make_udp_socket();
    c.peer.sin_family = AF_INET;
    c.peer.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &c.peer.sin_addr) != 1) {
      die_msg("inet_pton(raw_udp): invalid IPv4 address");
    }
    uint32_t id = next_id_++;
    conns_[id] = c;
    pollfds_.push_back(pollfd{c.fd, POLLIN, 0});
    poll_conn_ids_.push_back(id);
    return id;
  }

  bool is_connected(uint32_t) override { return true; }

  int send(uint32_t conn_id, const void* data, size_t len, bool /*reliable*/) override {
    if (len > MAX_UDP_PAYLOAD) return -1;
    if (server_fd_ >= 0) {
      auto it = peer_by_id_.find(conn_id);
      if (it == peer_by_id_.end()) return -1;
      return send_datagram(server_fd_, it->second, data, len);
    }
    auto it = conns_.find(conn_id);
    if (it == conns_.end()) return -1;
    return send_datagram(it->second.fd, it->second.peer, data, len);
  }

  int recv(void* buf, size_t cap, size_t* out_len, uint32_t* out_conn_id) override {
    if (server_fd_ >= 0) {
      sockaddr_in src{};
      ssize_t n = recv_datagram(server_fd_, buf, cap, &src);
      if (n == RECV_WOULD_BLOCK) return 0;
      if (n < 0) return -1;
      uint64_t k = addr_key(src);
      auto it = id_by_key_.find(k);
      uint32_t id;
      if (it == id_by_key_.end()) {
        id = next_id_++;
        id_by_key_[k] = id;
        peer_by_id_[id] = src;
      } else {
        id = it->second;
      }
      if (static_cast<size_t>(n) > cap) {
        *out_len = static_cast<size_t>(n);
        *out_conn_id = id;
        return -1;
      }
      *out_len = static_cast<size_t>(n);
      *out_conn_id = id;
      return 1;
    }
    return inbox_.recv(buf, cap, out_len, out_conn_id);
  }

  void poll() override {
    if (server_fd_ >= 0 || pollfds_.empty()) return;
    int ready = ::poll(pollfds_.data(), static_cast<nfds_t>(pollfds_.size()), 0);
    if (ready <= 0) return;

    uint8_t pkt[MAX_UDP_PAYLOAD];
    for (size_t i = 0; i < pollfds_.size() && ready > 0; ++i) {
      if ((pollfds_[i].revents & POLLIN) == 0) continue;
      --ready;
      for (;;) {
        ssize_t n = recv_client_datagram(pollfds_[i].fd, pkt, sizeof(pkt));
        if (n < 0) break;
        inbox_.enqueue(poll_conn_ids_[i], pkt, static_cast<size_t>(n));
      }
    }
  }

  void close() override {
    if (server_fd_ >= 0) { ::close(server_fd_); server_fd_ = -1; }
    for (auto& [id, c] : conns_) ::close(c.fd);
    conns_.clear();
    id_by_key_.clear();
    peer_by_id_.clear();
    pollfds_.clear();
    poll_conn_ids_.clear();
    inbox_.clear();
    next_id_ = 1;
  }

  const char* name() const override { return "raw_udp"; }
  bool supports(bool reliable) const override { return !reliable; }
  size_t max_payload_bytes(bool /*reliable*/) const override { return MAX_UDP_PAYLOAD; }
  const char* flush_policy(bool reliable) const override {
    return reliable ? "unsupported" : "immediate";
  }
  bool encryption_on() const override { return false; }

 private:
  int server_fd_ = -1;
  std::unordered_map<uint32_t, Conn> conns_;
  std::unordered_map<uint64_t, uint32_t> id_by_key_;
  std::unordered_map<uint32_t, sockaddr_in> peer_by_id_;
  std::vector<pollfd> pollfds_;
  std::vector<uint32_t> poll_conn_ids_;
  rudp_bench::ReusableInboundQueue inbox_;
  uint32_t next_id_ = 1;
};

}  // namespace

namespace rudp_bench {
void register_raw_udp_adapter() {
  register_adapter("raw_udp",
      []() { return std::make_unique<RawUdpAdapter>(); });
}
}  // namespace rudp_bench
