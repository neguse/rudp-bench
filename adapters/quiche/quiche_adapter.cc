#include "harness/adapter.h"
#include "harness/adapter_registry.h"
#include "harness/inbound_queue.h"

#include <quiche.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr size_t MAX_DATAGRAM_SIZE = 1350;
constexpr size_t kInboxLimit = 1u << 16;

struct CertPaths {
  std::string cert;
  std::string key;
};

const CertPaths& ensure_cert() {
  static CertPaths paths;
  static std::once_flag flag;
  std::call_once(flag, []() {
    char cert_path[128];
    char key_path[128];
    std::snprintf(cert_path, sizeof(cert_path),
                  "/tmp/rudp-bench-quiche-%ld-cert.pem",
                  static_cast<long>(::getpid()));
    std::snprintf(key_path, sizeof(key_path),
                  "/tmp/rudp-bench-quiche-%ld-key.pem",
                  static_cast<long>(::getpid()));
    paths.cert = cert_path;
    paths.key = key_path;
    std::string cmd =
        "openssl req -x509 -newkey rsa:2048 -nodes -days 365 "
        "-keyout " +
        paths.key + " -out " + paths.cert +
        " -subj '/CN=rudp-bench' 2>/dev/null";
    if (std::system(cmd.c_str()) != 0) std::abort();
  });
  return paths;
}

int make_nonblocking_udp(uint16_t port, bool bind_it) {
  int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) std::abort();
  int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int buf = 256 * 1024;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
  ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
  if (bind_it) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
      std::abort();
  }
  return fd;
}

void get_local_addr(int fd, sockaddr_storage* out, socklen_t* out_len) {
  *out_len = sizeof(*out);
  std::memset(out, 0, sizeof(*out));
  ::getsockname(fd, reinterpret_cast<sockaddr*>(out), out_len);
}

struct QuicheConn {
  quiche_conn* conn = nullptr;
  uint32_t id = 0;
  sockaddr_storage peer_addr{};
  socklen_t peer_len = 0;
  bool established = false;
  bool closed = false;
  uint64_t next_stream_id = 0;
  // per-connection stream framing state
  std::vector<uint8_t> stream_recv_buf;
  size_t stream_recv_offset = 0;
  uint32_t frame_len = 0;
  bool have_frame_len = false;
};

class QuicheAdapter : public rudp_bench::Adapter {
 public:
  QuicheAdapter() { inbox_.set_limit(kInboxLimit); }
  ~QuicheAdapter() override { close(); }

  void server_listen(uint16_t port) override {
    is_server_ = true;
    sock_fd_ = make_nonblocking_udp(port, true);
    get_local_addr(sock_fd_, &local_addr_, &local_len_);
    config_ = make_config(true);
  }

  uint32_t client_connect(const char* host, uint16_t port) override {
    if (sock_fd_ < 0) {
      sock_fd_ = make_nonblocking_udp(0, true);
      get_local_addr(sock_fd_, &local_addr_, &local_len_);
      config_ = make_config(false);
    }

    sockaddr_in peer_addr{};
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(port);
    ::inet_pton(AF_INET, host, &peer_addr.sin_addr);

    uint8_t scid[QUICHE_MAX_CONN_ID_LEN];
    gen_cid(scid, sizeof(scid));

    quiche_conn* qconn = quiche_connect(
        host, scid, sizeof(scid),
        reinterpret_cast<sockaddr*>(&local_addr_), local_len_,
        reinterpret_cast<sockaddr*>(&peer_addr),
        static_cast<socklen_t>(sizeof(peer_addr)), config_);
    if (!qconn) std::abort();

    auto c = std::make_unique<QuicheConn>();
    c->conn = qconn;
    c->id = next_id_++;
    std::memcpy(&c->peer_addr, &peer_addr, sizeof(peer_addr));
    c->peer_len = sizeof(peer_addr);
    // client uses bidi stream 0 for reliable
    c->next_stream_id = 0;

    uint32_t id = c->id;
    conn_by_id_[id] = c.get();

    const uint8_t* src_id = nullptr;
    size_t src_id_len = 0;
    quiche_conn_source_id(qconn, &src_id, &src_id_len);
    std::string key(reinterpret_cast<const char*>(src_id), src_id_len);
    conn_by_scid_[key] = c.get();

    conns_.push_back(std::move(c));
    return id;
  }

  bool is_connected(uint32_t conn_id) override {
    auto it = conn_by_id_.find(conn_id);
    if (it == conn_by_id_.end()) return false;
    return it->second->established && !it->second->closed;
  }

  int send(uint32_t conn_id, const void* data, size_t len,
           bool reliable) override {
    auto it = conn_by_id_.find(conn_id);
    if (it == conn_by_id_.end()) return -1;
    QuicheConn* c = it->second;
    if (!c->established || c->closed) return -1;

    if (reliable) {
      return send_stream(c, data, len);
    } else {
      return send_dgram(c, data, len);
    }
  }

  int recv(void* buf, size_t cap, size_t* out_len,
           uint32_t* out_conn_id) override {
    return inbox_.recv(buf, cap, out_len, out_conn_id);
  }

  void poll() override {
    if (sock_fd_ < 0) return;
    recv_packets();
    process_conns();
    flush_conns();
  }

  void close() override {
    if (closed_) return;
    closed_ = true;
    if (inbox_.dropped() > 0) {
      std::fprintf(stderr, "quiche_inbox_dropped: %llu\n",
                   (unsigned long long)inbox_.dropped());
      std::fflush(stderr);
    }
    for (auto& c : conns_) {
      if (c->conn) {
        quiche_conn_free(c->conn);
        c->conn = nullptr;
      }
    }
    if (config_) {
      quiche_config_free(config_);
      config_ = nullptr;
    }
    if (sock_fd_ >= 0) {
      ::close(sock_fd_);
      sock_fd_ = -1;
    }
  }

  const char* name() const override { return "quiche"; }
  bool supports(bool) const override { return true; }
  size_t max_payload_bytes(bool reliable) const override {
    return reliable ? 65536 : 1200;
  }
  const char* flush_policy(bool) const override { return "poll_send"; }
  bool encryption_on() const override { return true; }
  rudp_bench::ConnectionStats connection_stats() const override {
    return stats_;
  }

 private:
  quiche_config* make_config(bool is_server) {
    quiche_config* cfg = quiche_config_new(QUICHE_PROTOCOL_VERSION);
    if (!cfg) std::abort();

    const char* alpn = "\x09rudp-bnch";
    quiche_config_set_application_protos(cfg,
                                         reinterpret_cast<const uint8_t*>(alpn),
                                         std::strlen(alpn));

    if (is_server) {
      const auto& cert = ensure_cert();
      quiche_config_load_cert_chain_from_pem_file(cfg, cert.cert.c_str());
      quiche_config_load_priv_key_from_pem_file(cfg, cert.key.c_str());
    }
    quiche_config_verify_peer(cfg, false);

    quiche_config_set_max_idle_timeout(cfg, 30000);
    quiche_config_set_max_recv_udp_payload_size(cfg, MAX_DATAGRAM_SIZE);
    quiche_config_set_max_send_udp_payload_size(cfg, MAX_DATAGRAM_SIZE);
    quiche_config_set_initial_max_data(cfg, 10 * 1024 * 1024);
    quiche_config_set_initial_max_stream_data_bidi_local(cfg, 1024 * 1024);
    quiche_config_set_initial_max_stream_data_bidi_remote(cfg, 1024 * 1024);
    quiche_config_set_initial_max_stream_data_uni(cfg, 1024 * 1024);
    quiche_config_set_initial_max_streams_bidi(cfg, 16384);
    quiche_config_set_initial_max_streams_uni(cfg, 16384);
    quiche_config_set_cc_algorithm(cfg, QUICHE_CC_BBR2_GCONGESTION);
    quiche_config_enable_dgram(cfg, true, 1200, 65536);
    quiche_config_set_active_connection_id_limit(cfg, 8);

    return cfg;
  }

  void gen_cid(uint8_t* out, size_t len) {
    std::uniform_int_distribution<int> dist(0, 255);
    for (size_t i = 0; i < len; ++i) out[i] = static_cast<uint8_t>(dist(rng_));
  }

  void recv_packets() {
    constexpr unsigned kBatch = 64;
    uint8_t bufs[kBatch][MAX_DATAGRAM_SIZE];
    sockaddr_storage peers[kBatch];
    struct iovec iovs[kBatch];
    struct mmsghdr hdrs[kBatch];
    for (unsigned i = 0; i < kBatch; ++i) {
      iovs[i].iov_base = bufs[i];
      iovs[i].iov_len = sizeof(bufs[0]);
      std::memset(&hdrs[i], 0, sizeof(hdrs[0]));
      hdrs[i].msg_hdr.msg_name = &peers[i];
      hdrs[i].msg_hdr.msg_namelen = sizeof(peers[i]);
      hdrs[i].msg_hdr.msg_iov = &iovs[i];
      hdrs[i].msg_hdr.msg_iovlen = 1;
    }
    for (;;) {
      int rc = ::recvmmsg(sock_fd_, hdrs, kBatch, MSG_DONTWAIT, nullptr);
      if (rc <= 0) break;
      for (int i = 0; i < rc; ++i) {
        if (is_server_) {
          recv_server_packet(bufs[i], hdrs[i].msg_len,
                             reinterpret_cast<sockaddr*>(&peers[i]),
                             hdrs[i].msg_hdr.msg_namelen);
        } else {
          recv_client_packet(bufs[i], hdrs[i].msg_len,
                             reinterpret_cast<sockaddr*>(&peers[i]),
                             hdrs[i].msg_hdr.msg_namelen);
        }
      }
    }
  }

  void recv_server_packet(uint8_t* buf, size_t len, sockaddr* peer,
                          socklen_t peer_len) {
    uint8_t type = 0;
    uint32_t version = 0;
    uint8_t scid[QUICHE_MAX_CONN_ID_LEN];
    size_t scid_len = sizeof(scid);
    uint8_t dcid[QUICHE_MAX_CONN_ID_LEN];
    size_t dcid_len = sizeof(dcid);
    uint8_t token[256];
    size_t token_len = sizeof(token);

    int rc = quiche_header_info(buf, len, QUICHE_MAX_CONN_ID_LEN, &version,
                                &type, scid, &scid_len, dcid, &dcid_len,
                                token, &token_len);
    if (rc < 0) return;

    // Look up connection by DCID
    std::string key(reinterpret_cast<char*>(dcid), dcid_len);
    auto it = conn_by_scid_.find(key);
    QuicheConn* c = nullptr;

    if (it != conn_by_scid_.end()) {
      c = it->second;
    } else {
      if (!quiche_version_is_supported(version)) {
        uint8_t out[MAX_DATAGRAM_SIZE];
        ssize_t written = quiche_negotiate_version(scid, scid_len, dcid,
                                                    dcid_len, out, sizeof(out));
        if (written > 0) {
          ::sendto(sock_fd_, out, static_cast<size_t>(written), 0, peer,
                   peer_len);
        }
        return;
      }

      // New connection
      uint8_t new_scid[QUICHE_MAX_CONN_ID_LEN];
      gen_cid(new_scid, sizeof(new_scid));

      quiche_conn* qconn = quiche_accept(
          new_scid, sizeof(new_scid), nullptr, 0,
          reinterpret_cast<sockaddr*>(&local_addr_), local_len_, peer,
          peer_len, config_);
      if (!qconn) return;

      auto conn = std::make_unique<QuicheConn>();
      conn->conn = qconn;
      conn->id = next_id_++;
      std::memcpy(&conn->peer_addr, peer, peer_len);
      conn->peer_len = peer_len;
      // server uses bidi stream 1 for reliable (or relies on client-initiated)
      conn->next_stream_id = 1;

      c = conn.get();
      conn_by_id_[c->id] = c;

      std::string new_key(reinterpret_cast<char*>(new_scid),
                          sizeof(new_scid));
      conn_by_scid_[new_key] = c;
      conns_.push_back(std::move(conn));
    }

    quiche_recv_info info{};
    info.from = peer;
    info.from_len = peer_len;
    info.to = reinterpret_cast<sockaddr*>(&local_addr_);
    info.to_len = local_len_;

    quiche_conn_recv(c->conn, buf, len, &info);
  }

  void recv_client_packet(uint8_t* buf, size_t len, sockaddr* peer,
                          socklen_t peer_len) {
    uint8_t type = 0;
    uint32_t version = 0;
    uint8_t scid[QUICHE_MAX_CONN_ID_LEN];
    size_t scid_len = sizeof(scid);
    uint8_t dcid[QUICHE_MAX_CONN_ID_LEN];
    size_t dcid_len = sizeof(dcid);
    uint8_t token[256];
    size_t token_len = sizeof(token);

    int rc = quiche_header_info(buf, len, QUICHE_MAX_CONN_ID_LEN, &version,
                                &type, scid, &scid_len, dcid, &dcid_len,
                                token, &token_len);
    if (rc < 0) return;

    std::string key(reinterpret_cast<char*>(dcid), dcid_len);
    auto it = conn_by_scid_.find(key);
    if (it == conn_by_scid_.end()) return;

    quiche_recv_info info{};
    info.from = peer;
    info.from_len = peer_len;
    info.to = reinterpret_cast<sockaddr*>(&local_addr_);
    info.to_len = local_len_;
    quiche_conn_recv(it->second->conn, buf, len, &info);
  }

  void process_conns() {
    for (auto& c : conns_) {
      if (!c->conn || c->closed) continue;

      // Check establishment
      if (!c->established && quiche_conn_is_established(c->conn)) {
        c->established = true;
        ++connected_current_;
        if (connected_current_ > stats_.connected_peak) {
          stats_.connected_peak = connected_current_;
        }
      }

      uint8_t dgram_buf[MAX_DATAGRAM_SIZE];
      for (;;) {
        ssize_t dglen =
            quiche_conn_dgram_recv(c->conn, dgram_buf, sizeof(dgram_buf));
        if (dglen < 0) break;
        inbox_.enqueue(c->id, dgram_buf, static_cast<size_t>(dglen));
      }

      // Drain readable streams
      quiche_stream_iter* iter = quiche_conn_readable(c->conn);
      if (iter) {
        uint64_t stream_id;
        while (quiche_stream_iter_next(iter, &stream_id)) {
          uint8_t stream_buf[65536];
          bool fin = false;
          uint64_t err_code = 0;
          for (;;) {
            ssize_t slen = quiche_conn_stream_recv(
                c->conn, stream_id, stream_buf, sizeof(stream_buf), &fin,
                &err_code);
            if (slen <= 0) break;
            c->stream_recv_buf.insert(c->stream_recv_buf.end(), stream_buf,
                                      stream_buf + slen);
          }
          drain_frames(c.get());
        }
        quiche_stream_iter_free(iter);
      }

      // Handle timeout
      if (quiche_conn_timeout_as_millis(c->conn) == 0) {
        quiche_conn_on_timeout(c->conn);
      }

      // Handle closed — mark so we skip this conn on future ticks
      if (quiche_conn_is_closed(c->conn)) {
        c->closed = true;
        if (c->established) {
          c->established = false;
          if (connected_current_ > 0) --connected_current_;
          ++stats_.shutdown_by_transport;
        }
      }
    }
  }

  void flush_conns() {
    constexpr unsigned kBatch = 64;
    uint8_t bufs[kBatch][MAX_DATAGRAM_SIZE];
    struct iovec iovs[kBatch];
    sockaddr_storage addrs[kBatch];
    struct mmsghdr hdrs[kBatch];
    unsigned pending = 0;

    auto flush_batch = [&]() {
      if (pending == 0) return;
      int rc = ::sendmmsg(sock_fd_, hdrs, pending, 0);
      (void)rc;
      pending = 0;
    };

    for (auto& c : conns_) {
      if (!c->conn || c->closed) continue;
      for (;;) {
        quiche_send_info send_info{};
        ssize_t written =
            quiche_conn_send(c->conn, bufs[pending], sizeof(bufs[0]), &send_info);
        if (written == QUICHE_ERR_DONE) break;
        if (written < 0) break;
        iovs[pending].iov_base = bufs[pending];
        iovs[pending].iov_len = static_cast<size_t>(written);
        std::memcpy(&addrs[pending], &send_info.to, send_info.to_len);
        std::memset(&hdrs[pending], 0, sizeof(hdrs[0]));
        hdrs[pending].msg_hdr.msg_name = &addrs[pending];
        hdrs[pending].msg_hdr.msg_namelen = send_info.to_len;
        hdrs[pending].msg_hdr.msg_iov = &iovs[pending];
        hdrs[pending].msg_hdr.msg_iovlen = 1;
        if (++pending == kBatch) flush_batch();
      }
    }
    flush_batch();
  }

  int send_stream(QuicheConn* c, const void* data, size_t len) {
    // Length-prefix framing (4-byte big-endian + payload), same as msquic
    uint64_t stream_id = c->next_stream_id;
    uint64_t err_code = 0;

    // Combine header + payload into single buffer to avoid partial writes
    std::vector<uint8_t> frame(4 + len);
    uint32_t nlen = htonl(static_cast<uint32_t>(len));
    std::memcpy(frame.data(), &nlen, 4);
    std::memcpy(frame.data() + 4, data, len);

    ssize_t sent = quiche_conn_stream_send(
        c->conn, stream_id, frame.data(), frame.size(), false, &err_code);
    if (sent < 0) return -1;
    return 0;
  }

  int send_dgram(QuicheConn* c, const void* data, size_t len) {
    ssize_t rc = quiche_conn_dgram_send(
        c->conn, static_cast<const uint8_t*>(data), len);
    return rc >= 0 ? 0 : -1;
  }

  void drain_frames(QuicheConn* c) {
    auto& rb = c->stream_recv_buf;
    auto available = [&]() -> size_t { return rb.size() - c->stream_recv_offset; };
    while (true) {
      if (!c->have_frame_len) {
        if (available() < 4) break;
        uint32_t nlen;
        std::memcpy(&nlen, rb.data() + c->stream_recv_offset, 4);
        c->stream_recv_offset += 4;
        c->frame_len = ntohl(nlen);
        c->have_frame_len = true;
      }
      if (available() < c->frame_len) break;
      inbox_.enqueue(c->id, rb.data() + c->stream_recv_offset, c->frame_len);
      c->stream_recv_offset += c->frame_len;
      c->have_frame_len = false;
    }
    if (c->stream_recv_offset == rb.size()) {
      rb.clear();
      c->stream_recv_offset = 0;
    } else if (c->stream_recv_offset > 4096 &&
               c->stream_recv_offset * 2 >= rb.size()) {
      rb.erase(rb.begin(),
               rb.begin() +
                   static_cast<std::ptrdiff_t>(c->stream_recv_offset));
      c->stream_recv_offset = 0;
    }
  }

  bool is_server_ = false;
  bool closed_ = false;
  int sock_fd_ = -1;
  quiche_config* config_ = nullptr;
  sockaddr_storage local_addr_{};
  socklen_t local_len_ = 0;

  std::vector<std::unique_ptr<QuicheConn>> conns_;
  std::unordered_map<uint32_t, QuicheConn*> conn_by_id_;
  std::unordered_map<std::string, QuicheConn*> conn_by_scid_;
  uint32_t next_id_ = 1;
  uint32_t connected_current_ = 0;
  rudp_bench::ConnectionStats stats_;
  rudp_bench::ReusableInboundQueue inbox_;
  std::mt19937 rng_{std::random_device{}()};
};

}  // namespace

namespace rudp_bench {
void register_quiche_adapter() {
  register_adapter("quiche",
                   []() { return std::make_unique<QuicheAdapter>(); });
}
}  // namespace rudp_bench
