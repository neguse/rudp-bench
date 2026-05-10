#include "harness/adapter.h"
#include "harness/adapter_registry.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <unordered_map>
#include <vector>

namespace {

struct Conn {
  int fd = -1;
  sockaddr_in peer{};
};

uint64_t addr_key(const sockaddr_in& a) {
  return (static_cast<uint64_t>(a.sin_addr.s_addr) << 32) | static_cast<uint64_t>(a.sin_port);
}

void set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

class RawUdpAdapter : public rudp_bench::Adapter {
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
    conns_[id] = c;
    return id;
  }

  bool is_connected(uint32_t) override { return true; }

  int send(uint32_t conn_id, const void* data, size_t len, bool /*reliable*/) override {
    if (server_fd_ >= 0) {
      auto it = peer_by_id_.find(conn_id);
      if (it == peer_by_id_.end()) return -1;
      ssize_t n = ::sendto(server_fd_, data, len, 0,
                          reinterpret_cast<sockaddr*>(&it->second), sizeof(it->second));
      return (n == static_cast<ssize_t>(len)) ? 0 : -1;
    }
    auto it = conns_.find(conn_id);
    if (it == conns_.end()) return -1;
    ssize_t n = ::sendto(it->second.fd, data, len, 0,
                        reinterpret_cast<sockaddr*>(&it->second.peer),
                        sizeof(it->second.peer));
    return (n == static_cast<ssize_t>(len)) ? 0 : -1;
  }

  int recv(void* buf, size_t cap, size_t* out_len, uint32_t* out_conn_id) override {
    if (server_fd_ >= 0) {
      sockaddr_in src{};
      socklen_t sl = sizeof(src);
      ssize_t n = ::recvfrom(server_fd_, buf, cap, 0,
                            reinterpret_cast<sockaddr*>(&src), &sl);
      if (n <= 0) return 0;
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
      *out_len = static_cast<size_t>(n);
      *out_conn_id = id;
      return 1;
    }
    for (auto& [id, c] : conns_) {
      ssize_t n = ::recv(c.fd, buf, cap, 0);
      if (n > 0) {
        *out_len = static_cast<size_t>(n);
        *out_conn_id = id;
        return 1;
      }
    }
    return 0;
  }

  void poll() override {}

  void close() override {
    if (server_fd_ >= 0) { ::close(server_fd_); server_fd_ = -1; }
    for (auto& [id, c] : conns_) ::close(c.fd);
    conns_.clear();
  }

  const char* name() const override { return "raw_udp"; }
  bool supports(bool reliable) const override { return !reliable; }
  size_t max_payload_bytes(bool /*reliable*/) const override { return 65507; }
  const char* flush_policy(bool reliable) const override {
    return reliable ? "unsupported" : "immediate";
  }
  bool encryption_on() const override { return false; }

 private:
  int server_fd_ = -1;
  std::unordered_map<uint32_t, Conn> conns_;
  std::unordered_map<uint64_t, uint32_t> id_by_key_;
  std::unordered_map<uint32_t, sockaddr_in> peer_by_id_;
  uint32_t next_id_ = 1;
};

}  // namespace

namespace rudp_bench {
void register_raw_udp_adapter() {
  register_adapter("raw_udp",
      []() { return std::make_unique<RawUdpAdapter>(); });
}
}  // namespace rudp_bench
