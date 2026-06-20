#include "harness/adapter.h"
#include "harness/adapter_registry.h"
#include "harness/inbound_queue.h"

#include <lsquic.h>
#include "lsquic_cert_helper.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

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
                  "/tmp/rudp-bench-lsquic-%ld-cert.pem",
                  static_cast<long>(::getpid()));
    std::snprintf(key_path, sizeof(key_path),
                  "/tmp/rudp-bench-lsquic-%ld-key.pem",
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

class LsquicAdapter;

struct ConnCtx {
  LsquicAdapter* adapter;
  uint32_t id;
  lsquic_conn_t* conn;
  bool established;
  lsquic_stream_t* reliable_stream;
  // Pending reliable send queue
  std::deque<std::vector<uint8_t>> pending_writes;
  bool want_write;
  // Per-connection datagram send queue (avoids global linear scan)
  std::deque<std::vector<uint8_t>> dgram_queue;
  // Stream framing receive state
  std::vector<uint8_t> stream_recv_buf;
  size_t stream_recv_offset;
  uint32_t frame_len;
  bool have_frame_len;
};

struct StreamCtx {
  ConnCtx* conn_ctx;
  bool is_reliable;
};

class LsquicAdapter : public rudp_bench::Adapter {
 public:
  LsquicAdapter() {
    inbox_.set_limit(kInboxLimit);
    static std::once_flag flag;
    std::call_once(flag, []() {
      if (lsquic_global_init(LSQUIC_GLOBAL_CLIENT | LSQUIC_GLOBAL_SERVER) !=
          0) {
        std::abort();
      }
      static const lsquic_logger_if logger_if = {
          [](void*, const char* buf, size_t len) -> int {
            std::fwrite(buf, 1, len, stderr);
            std::fputc('\n', stderr);
            return 0;
          }};
      lsquic_set_log_level("error");
      lsquic_logger_init(&logger_if, nullptr, LLTS_HHMMSSUS);
    });
  }
  ~LsquicAdapter() override { close(); }

  void server_listen(uint16_t port) override {
    is_server_ = true;
    port_ = port;
    sock_fd_ = make_udp_socket(port, true);
    get_local_addr();

    const auto& cert = ensure_cert();
    ssl_ctx_ = rudp_bench_lsquic_create_ssl_ctx(cert.cert.c_str(),
                                                  cert.key.c_str());

    create_engine(LSENG_SERVER);
  }

  uint32_t client_connect(const char* host, uint16_t port) override {
    if (sock_fd_ < 0) {
      sock_fd_ = make_udp_socket(0, true);
      get_local_addr();
      create_engine(0);
    }

    sockaddr_in peer_addr{};
    peer_addr.sin_family = AF_INET;
    peer_addr.sin_port = htons(port);
    ::inet_pton(AF_INET, host, &peer_addr.sin_addr);

    uint32_t id = next_id_++;
    auto* cctx = new ConnCtx{};
    cctx->adapter = this;
    cctx->id = id;
    cctx->established = false;
    cctx->reliable_stream = nullptr;
    cctx->want_write = false;
    cctx->stream_recv_offset = 0;
    cctx->frame_len = 0;
    cctx->have_frame_len = false;

    lsquic_conn_t* conn = lsquic_engine_connect(
        engine_, N_LSQVER,
        reinterpret_cast<sockaddr*>(&local_addr_),
        reinterpret_cast<sockaddr*>(&peer_addr), this,
        reinterpret_cast<lsquic_conn_ctx_t*>(cctx), nullptr, 0, nullptr, 0,
        nullptr, 0);
    if (!conn) {
      delete cctx;
      std::abort();
    }
    cctx->conn = conn;
    conn_by_id_[id] = cctx;
    lsquic_engine_process_conns(engine_);
    return id;
  }

  bool is_connected(uint32_t conn_id) override {
    auto it = conn_by_id_.find(conn_id);
    if (it == conn_by_id_.end()) return false;
    return it->second->established;
  }

  int send(uint32_t conn_id, const void* data, size_t len,
           bool reliable) override {
    auto it = conn_by_id_.find(conn_id);
    if (it == conn_by_id_.end()) return -1;
    ConnCtx* cctx = it->second;
    if (!cctx->established) return -1;

    if (reliable) {
      return send_reliable(cctx, data, len);
    } else {
      return send_dgram(cctx, data, len);
    }
  }

  int recv(void* buf, size_t cap, size_t* out_len,
           uint32_t* out_conn_id) override {
    return inbox_.recv(buf, cap, out_len, out_conn_id);
  }

  void poll() override {
    if (sock_fd_ < 0 || !engine_) return;
    recv_packets();
    lsquic_engine_process_conns(engine_);
    if (lsquic_engine_has_unsent_packets(engine_)) {
      lsquic_engine_send_unsent_packets(engine_);
    }
  }

  void close() override {
    if (closed_) return;
    closed_ = true;
    if (inbox_.dropped() > 0) {
      std::fprintf(stderr, "lsquic_inbox_dropped: %llu\n",
                   (unsigned long long)inbox_.dropped());
      std::fflush(stderr);
    }
    // Don't try to cleanly shut down — same rationale as msquic adapter
  }

  const char* name() const override { return "lsquic"; }
  bool supports(bool) const override { return true; }
  size_t max_payload_bytes(bool reliable) const override {
    return reliable ? 65536 : 1200;
  }
  const char* flush_policy(bool) const override {
    return "poll_process_conns";
  }
  bool encryption_on() const override { return true; }
  rudp_bench::ConnectionStats connection_stats() const override {
    return stats_;
  }

  // Public for static callbacks
  rudp_bench::ReusableInboundQueue inbox_;
  rudp_bench::ConnectionStats stats_;
  uint32_t connected_current_ = 0;

 private:
  static int make_udp_socket(uint16_t port, bool bind_it) {
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

  void get_local_addr() {
    local_len_ = sizeof(local_addr_);
    std::memset(&local_addr_, 0, sizeof(local_addr_));
    ::getsockname(sock_fd_, reinterpret_cast<sockaddr*>(&local_addr_),
                  &local_len_);
  }

  void create_engine(unsigned flags) {
    lsquic_engine_settings settings;
    lsquic_engine_init_settings(&settings, flags);
    settings.es_idle_timeout = 30;
    settings.es_init_max_data = 10 * 1024 * 1024;
    settings.es_init_max_stream_data_bidi_local = 1024 * 1024;
    settings.es_init_max_stream_data_bidi_remote = 1024 * 1024;
    settings.es_init_max_stream_data_uni = 1024 * 1024;
    settings.es_init_max_streams_bidi = 16384;
    settings.es_init_max_streams_uni = 16384;
    settings.es_datagrams = 1;
    settings.es_cc_algo = 2;  // BBR

    static const lsquic_stream_if stream_if = {
        .on_new_conn = cb_on_new_conn,
        .on_goaway_received = nullptr,
        .on_conn_closed = cb_on_conn_closed,
        .on_new_stream = cb_on_new_stream,
        .on_read = cb_on_read,
        .on_write = cb_on_write,
        .on_close = cb_on_close,
        .on_dg_write = cb_on_dg_write,
        .on_datagram = cb_on_datagram,
        .on_hsk_done = cb_on_hsk_done,
        .on_new_token = nullptr,
        .on_sess_resume_info = nullptr,
    };

    lsquic_engine_api api{};
    api.ea_settings = &settings;
    api.ea_stream_if = &stream_if;
    api.ea_stream_if_ctx = this;
    api.ea_packets_out = cb_packets_out;
    api.ea_packets_out_ctx = this;
    api.ea_alpn = "rudp-bnch";
    if (flags & LSENG_SERVER) {
      api.ea_get_ssl_ctx = cb_get_ssl_ctx;
    }

    engine_ = lsquic_engine_new(flags, &api);
    if (!engine_) std::abort();
  }

  void recv_packets() {
    uint8_t buf[65535];
    sockaddr_storage peer{};
    socklen_t peer_len;
    for (;;) {
      peer_len = sizeof(peer);
      ssize_t n = ::recvfrom(sock_fd_, buf, sizeof(buf), 0,
                             reinterpret_cast<sockaddr*>(&peer), &peer_len);
      if (n <= 0) break;
      lsquic_engine_packet_in(
          engine_, buf, static_cast<size_t>(n),
          reinterpret_cast<sockaddr*>(&local_addr_),
          reinterpret_cast<sockaddr*>(&peer), this, 0);
    }
  }

  int send_reliable(ConnCtx* cctx, const void* data, size_t len) {
    // Queue the length-prefixed frame
    std::vector<uint8_t> frame(4 + len);
    uint32_t nlen = htonl(static_cast<uint32_t>(len));
    std::memcpy(frame.data(), &nlen, 4);
    std::memcpy(frame.data() + 4, data, len);
    cctx->pending_writes.push_back(std::move(frame));

    if (cctx->reliable_stream && !cctx->want_write) {
      lsquic_stream_wantwrite(cctx->reliable_stream, 1);
      cctx->want_write = true;
    } else if (!cctx->reliable_stream) {
      lsquic_conn_make_stream(cctx->conn);
    }
    return 0;
  }

  int send_dgram(ConnCtx* cctx, const void* data, size_t len) {
    cctx->dgram_queue.emplace_back(
        static_cast<const uint8_t*>(data),
        static_cast<const uint8_t*>(data) + len);
    lsquic_conn_want_datagram_write(cctx->conn, 1);
    return 0;
  }

  static void drain_frames(ConnCtx* cctx) {
    auto& rb = cctx->stream_recv_buf;
    auto available = [&]() -> size_t {
      return rb.size() - cctx->stream_recv_offset;
    };
    while (true) {
      if (!cctx->have_frame_len) {
        if (available() < 4) break;
        uint32_t nlen;
        std::memcpy(&nlen, rb.data() + cctx->stream_recv_offset, 4);
        cctx->stream_recv_offset += 4;
        cctx->frame_len = ntohl(nlen);
        cctx->have_frame_len = true;
      }
      if (available() < cctx->frame_len) break;
      cctx->adapter->inbox_.enqueue(
          cctx->id, rb.data() + cctx->stream_recv_offset, cctx->frame_len);
      cctx->stream_recv_offset += cctx->frame_len;
      cctx->have_frame_len = false;
    }
    if (cctx->stream_recv_offset == rb.size()) {
      rb.clear();
      cctx->stream_recv_offset = 0;
    } else if (cctx->stream_recv_offset > 4096 &&
               cctx->stream_recv_offset * 2 >= rb.size()) {
      rb.erase(rb.begin(),
               rb.begin() +
                   static_cast<std::ptrdiff_t>(cctx->stream_recv_offset));
      cctx->stream_recv_offset = 0;
    }
  }

  // --- Static callbacks ---

  static lsquic_conn_ctx_t* cb_on_new_conn(void* stream_if_ctx,
                                             lsquic_conn_t* conn) {
    auto* adapter = static_cast<LsquicAdapter*>(stream_if_ctx);
    // Server side: on_new_conn means handshake is already done (mini-conn promoted)
    if (adapter->is_server_) {
      auto* cctx = new ConnCtx{};
      cctx->adapter = adapter;
      cctx->id = adapter->next_id_++;
      cctx->conn = conn;
      cctx->established = true;
      cctx->reliable_stream = nullptr;
      cctx->want_write = false;
      cctx->stream_recv_offset = 0;
      cctx->frame_len = 0;
      cctx->have_frame_len = false;
      adapter->conn_by_id_[cctx->id] = cctx;
      ++adapter->connected_current_;
      if (adapter->connected_current_ > adapter->stats_.connected_peak) {
        adapter->stats_.connected_peak = adapter->connected_current_;
      }
      return reinterpret_cast<lsquic_conn_ctx_t*>(cctx);
    }
    // Client side: ctx was set via lsquic_engine_connect
    return lsquic_conn_get_ctx(conn);
  }

  static void cb_on_conn_closed(lsquic_conn_t* conn) {
    auto* cctx =
        reinterpret_cast<ConnCtx*>(lsquic_conn_get_ctx(conn));
    if (!cctx) return;
    auto* adapter = cctx->adapter;
    if (cctx->established) {
      cctx->established = false;
      if (adapter->connected_current_ > 0) --adapter->connected_current_;
      ++adapter->stats_.shutdown_by_transport;
    }
    adapter->conn_by_id_.erase(cctx->id);
    delete cctx;
  }

  static lsquic_stream_ctx_t* cb_on_new_stream(void* stream_if_ctx,
                                                 lsquic_stream_t* stream) {
    (void)stream_if_ctx;
    if (!stream) return nullptr;

    // Find the conn ctx through the connection
    lsquic_conn_t* conn = lsquic_stream_conn(stream);
    auto* cctx = reinterpret_cast<ConnCtx*>(lsquic_conn_get_ctx(conn));
    if (!cctx) return nullptr;

    auto* sctx = new StreamCtx{cctx, true};

    if (!cctx->reliable_stream) {
      cctx->reliable_stream = stream;
    }

    lsquic_stream_wantread(stream, 1);
    if (!cctx->pending_writes.empty()) {
      lsquic_stream_wantwrite(stream, 1);
      cctx->want_write = true;
    }

    return reinterpret_cast<lsquic_stream_ctx_t*>(sctx);
  }

  static void cb_on_read(lsquic_stream_t* stream,
                          lsquic_stream_ctx_t* ctx) {
    auto* sctx = reinterpret_cast<StreamCtx*>(ctx);
    if (!sctx || !sctx->conn_ctx) return;
    auto* cctx = sctx->conn_ctx;

    uint8_t buf[65536];
    for (;;) {
      ssize_t n = lsquic_stream_read(stream, buf, sizeof(buf));
      if (n <= 0) break;
      cctx->stream_recv_buf.insert(cctx->stream_recv_buf.end(), buf,
                                    buf + n);
    }
    drain_frames(cctx);
  }

  static void cb_on_write(lsquic_stream_t* stream,
                           lsquic_stream_ctx_t* ctx) {
    auto* sctx = reinterpret_cast<StreamCtx*>(ctx);
    if (!sctx || !sctx->conn_ctx) return;
    auto* cctx = sctx->conn_ctx;

    while (!cctx->pending_writes.empty()) {
      auto& frame = cctx->pending_writes.front();
      ssize_t written =
          lsquic_stream_write(stream, frame.data(), frame.size());
      if (written <= 0) break;
      if (static_cast<size_t>(written) < frame.size()) {
        frame.erase(frame.begin(), frame.begin() + written);
        break;
      }
      cctx->pending_writes.pop_front();
    }
    lsquic_stream_flush(stream);

    if (cctx->pending_writes.empty()) {
      lsquic_stream_wantwrite(stream, 0);
      cctx->want_write = false;
    }
  }

  static void cb_on_close(lsquic_stream_t* stream,
                           lsquic_stream_ctx_t* ctx) {
    (void)stream;
    auto* sctx = reinterpret_cast<StreamCtx*>(ctx);
    if (!sctx) return;
    if (sctx->conn_ctx && sctx->conn_ctx->reliable_stream == stream) {
      sctx->conn_ctx->reliable_stream = nullptr;
      sctx->conn_ctx->want_write = false;
    }
    delete sctx;
  }

  static ssize_t cb_on_dg_write(lsquic_conn_t* conn, void* buf,
                                 size_t buf_sz) {
    auto* cctx =
        reinterpret_cast<ConnCtx*>(lsquic_conn_get_ctx(conn));
    if (!cctx) return -1;
    auto& q = cctx->dgram_queue;
    if (q.empty()) {
      lsquic_conn_want_datagram_write(conn, 0);
      return -1;
    }
    auto& front = q.front();
    if (front.size() > buf_sz) {
      q.pop_front();
      if (q.empty()) lsquic_conn_want_datagram_write(conn, 0);
      return -1;
    }
    std::memcpy(buf, front.data(), front.size());
    ssize_t len = static_cast<ssize_t>(front.size());
    q.pop_front();
    if (q.empty()) lsquic_conn_want_datagram_write(conn, 0);
    return len;
  }

  static void cb_on_datagram(lsquic_conn_t* conn, const void* buf,
                              size_t len) {
    auto* cctx =
        reinterpret_cast<ConnCtx*>(lsquic_conn_get_ctx(conn));
    if (!cctx) return;
    cctx->adapter->inbox_.enqueue(cctx->id,
                                   static_cast<const uint8_t*>(buf), len);
  }

  static void cb_on_hsk_done(lsquic_conn_t* conn,
                              enum lsquic_hsk_status status) {
    if (status != LSQ_HSK_OK && status != LSQ_HSK_RESUMED_OK) return;
    auto* cctx =
        reinterpret_cast<ConnCtx*>(lsquic_conn_get_ctx(conn));
    if (!cctx) return;
    cctx->established = true;
    auto* adapter = cctx->adapter;
    ++adapter->connected_current_;
    if (adapter->connected_current_ > adapter->stats_.connected_peak) {
      adapter->stats_.connected_peak = adapter->connected_current_;
    }
  }

  static int cb_packets_out(void* ctx,
                             const lsquic_out_spec* specs,
                             unsigned n_specs) {
    auto* adapter = static_cast<LsquicAdapter*>(ctx);
    constexpr unsigned kBatch = 64;
    struct mmsghdr hdrs[kBatch];
    unsigned n_sent = 0;
    while (n_sent < n_specs) {
      unsigned batch = std::min(kBatch, n_specs - n_sent);
      for (unsigned i = 0; i < batch; ++i) {
        auto& s = specs[n_sent + i];
        auto& h = hdrs[i];
        std::memset(&h, 0, sizeof(h));
        h.msg_hdr.msg_name = const_cast<sockaddr*>(s.dest_sa);
        h.msg_hdr.msg_namelen = (s.dest_sa->sa_family == AF_INET6)
                                    ? sizeof(sockaddr_in6)
                                    : sizeof(sockaddr_in);
        h.msg_hdr.msg_iov = s.iov;
        h.msg_hdr.msg_iovlen = s.iovlen;
      }
      int rc = ::sendmmsg(adapter->sock_fd_, hdrs, batch, 0);
      if (rc <= 0) {
        if (n_sent == 0) return -1;
        break;
      }
      n_sent += static_cast<unsigned>(rc);
      if (static_cast<unsigned>(rc) < batch) break;
    }
    return static_cast<int>(n_sent);
  }

  static ssl_ctx_st* cb_get_ssl_ctx(void* peer_ctx,
                                     const sockaddr* /*local*/) {
    auto* adapter = static_cast<LsquicAdapter*>(peer_ctx);
    return adapter->ssl_ctx_;
  }

  bool is_server_ = false;
  bool closed_ = false;
  int sock_fd_ = -1;
  uint16_t port_ = 0;
  lsquic_engine_t* engine_ = nullptr;
  ssl_ctx_st* ssl_ctx_ = nullptr;
  sockaddr_storage local_addr_{};
  socklen_t local_len_ = 0;

  std::unordered_map<uint32_t, ConnCtx*> conn_by_id_;
  uint32_t next_id_ = 1;
};

}  // namespace

namespace rudp_bench {
void register_lsquic_adapter() {
  register_adapter("lsquic",
                   []() { return std::make_unique<LsquicAdapter>(); });
}
}  // namespace rudp_bench
