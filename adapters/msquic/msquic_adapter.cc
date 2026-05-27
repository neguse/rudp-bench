#include "harness/adapter.h"
#include "harness/adapter_registry.h"
#include "harness/inbound_queue.h"

#include <msquic.h>

#include <arpa/inet.h>
#include <atomic>
#include <cstddef>
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

const QUIC_API_TABLE* MsQuic = nullptr;

#define MSQUIC_DIE(msg, status) do { \
  std::fprintf(stderr, "msquic_adapter: " msg " status=0x%x at %s:%d\n", \
               (unsigned)(status), __FILE__, __LINE__); \
  std::fflush(stderr); \
  std::abort(); \
} while (0)

void ensure_msquic_init() {
  static std::once_flag flag;
  std::call_once(flag, []() {
    if (QUIC_STATUS s = MsQuicOpen2(&MsQuic); QUIC_FAILED(s)) MSQUIC_DIE("MsQuicOpen2", s);
    // Intentionally NOT registering atexit MsQuicClose: at 200+ conns the
    // global teardown races with msquic worker threads still finishing
    // callbacks and triggers double-free in glibc. We rely on the OS to
    // reclaim resources at process exit.
  });
}

void ensure_msquic_cert() {
  static std::once_flag flag;
  std::call_once(flag, []() {
    // Generate self-signed cert for benchmark use
    int rc = std::system(
        "openssl req -x509 -newkey rsa:2048 -nodes -days 365 "
        "-keyout /tmp/msquic_key.pem -out /tmp/msquic_cert.pem "
        "-subj '/CN=rudp-bench' 2>/dev/null");
    if (rc != 0) std::abort();
  });
}

const QUIC_BUFFER Alpn = {9, (uint8_t*)"rudp-bnch"};

struct SendCtx {
  std::vector<uint8_t> data;
  QUIC_BUFFER buf;
};

struct StreamCtx {
  class MsquicAdapter* adapter;
  uint32_t conn_id;
  HQUIC conn;
  bool outbound;
  std::vector<uint8_t> recv_buf;
  size_t recv_offset = 0;
  uint32_t frame_len = 0;
  bool have_len = false;

  StreamCtx(class MsquicAdapter* adapter, uint32_t conn_id, HQUIC conn, bool outbound)
      : adapter(adapter), conn_id(conn_id), conn(conn), outbound(outbound) {}
};

class MsquicAdapter : public rudp_bench::Adapter {
 public:
  MsquicAdapter() { ensure_msquic_init(); }
  ~MsquicAdapter() override { close(); }

  void server_listen(uint16_t port) override {
    is_server_ = true;
    ensure_msquic_cert();

    QUIC_REGISTRATION_CONFIG reg_cfg = {"rudp-bench-srv", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
    QUIC_STATUS status;
    status = MsQuic->RegistrationOpen(&reg_cfg, &reg_);
    if (QUIC_FAILED(status)) MSQUIC_DIE("server RegistrationOpen", status);

    QUIC_SETTINGS settings{};
    settings.IdleTimeoutMs = 30000;
    settings.IsSet.IdleTimeoutMs = TRUE;
    settings.PeerBidiStreamCount = 16384;
    settings.IsSet.PeerBidiStreamCount = TRUE;
    settings.PeerUnidiStreamCount = 16384;
    settings.IsSet.PeerUnidiStreamCount = TRUE;
    settings.DatagramReceiveEnabled = TRUE;
    settings.IsSet.DatagramReceiveEnabled = TRUE;

    status = MsQuic->ConfigurationOpen(reg_, &Alpn, 1, &settings, sizeof(settings), nullptr, &config_);
    if (QUIC_FAILED(status)) MSQUIC_DIE("server ConfigurationOpen", status);

    QUIC_CERTIFICATE_FILE cert_file;
    cert_file.CertificateFile = "/tmp/msquic_cert.pem";
    cert_file.PrivateKeyFile = "/tmp/msquic_key.pem";

    QUIC_CREDENTIAL_CONFIG cred_cfg{};
    cred_cfg.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    cred_cfg.CertificateFile = &cert_file;
    cred_cfg.Flags = QUIC_CREDENTIAL_FLAG_NONE;

    status = MsQuic->ConfigurationLoadCredential(config_, &cred_cfg);
    if (QUIC_FAILED(status)) MSQUIC_DIE("server ConfigurationLoadCredential", status);

    status = MsQuic->ListenerOpen(reg_, listener_cb, this, &listener_);
    if (QUIC_FAILED(status)) MSQUIC_DIE("server ListenerOpen", status);

    QUIC_ADDR addr{};
    std::memset(&addr, 0, sizeof(addr));
    QuicAddrSetFamily(&addr, QUIC_ADDRESS_FAMILY_UNSPEC);
    QuicAddrSetPort(&addr, port);

    status = MsQuic->ListenerStart(listener_, &Alpn, 1, &addr);
    if (QUIC_FAILED(status)) MSQUIC_DIE("server ListenerStart", status);
  }

  uint32_t client_connect(const char* host, uint16_t port) override {
    if (!reg_) {
      QUIC_REGISTRATION_CONFIG reg_cfg = {"rudp-bench-cli", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
      if (QUIC_STATUS s = MsQuic->RegistrationOpen(&reg_cfg, &reg_); QUIC_FAILED(s)) MSQUIC_DIE("client RegistrationOpen", s);

      QUIC_SETTINGS settings{};
      settings.IdleTimeoutMs = 30000;
      settings.IsSet.IdleTimeoutMs = TRUE;
      settings.PeerBidiStreamCount = 16384;
      settings.IsSet.PeerBidiStreamCount = TRUE;
      settings.PeerUnidiStreamCount = 16384;
      settings.IsSet.PeerUnidiStreamCount = TRUE;
      settings.DatagramReceiveEnabled = TRUE;
      settings.IsSet.DatagramReceiveEnabled = TRUE;

      if (QUIC_STATUS s = MsQuic->ConfigurationOpen(
              reg_, &Alpn, 1, &settings, sizeof(settings), nullptr, &config_);
          QUIC_FAILED(s))
        MSQUIC_DIE("client ConfigurationOpen", s);

      QUIC_CREDENTIAL_CONFIG cred_cfg{};
      cred_cfg.Type = QUIC_CREDENTIAL_TYPE_NONE;
      cred_cfg.Flags = QUIC_CREDENTIAL_FLAG_CLIENT |
                        QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;

      if (QUIC_STATUS s = MsQuic->ConfigurationLoadCredential(config_, &cred_cfg);
          QUIC_FAILED(s))
        MSQUIC_DIE("client ConfigurationLoadCredential", s);
    }

    HQUIC conn = nullptr;
    if (QUIC_STATUS s = MsQuic->ConnectionOpen(reg_, connection_cb, this, &conn);
        QUIC_FAILED(s))
      MSQUIC_DIE("client ConnectionOpen", s);

    uint32_t id;
    {
      std::lock_guard<std::mutex> lock(mtx_);
      id = next_id_++;
      id_to_conn_[id] = conn;
      conn_to_id_[conn] = id;
    }

    if (QUIC_STATUS s = MsQuic->ConnectionStart(
            conn, config_, QUIC_ADDRESS_FAMILY_UNSPEC, host, port);
        QUIC_FAILED(s)) {
      MSQUIC_DIE("client ConnectionStart", s);
    }

    return id;
  }

  bool is_connected(uint32_t conn_id) override {
    std::lock_guard<std::mutex> lock(mtx_);
    return connected_ids_.count(conn_id) > 0;
  }

  int send(uint32_t conn_id, const void* data, size_t len, bool reliable) override {
    HQUIC conn = nullptr;
    {
      std::lock_guard<std::mutex> lock(mtx_);
      auto it = id_to_conn_.find(conn_id);
      if (it == id_to_conn_.end()) return -1;
      conn = it->second;
    }

    if (reliable) {
      return send_stream(conn, data, len);
    } else {
      return send_datagram(conn, data, len);
    }
  }

  int recv(void* buf, size_t cap, size_t* out_len, uint32_t* out_conn_id) override {
    std::lock_guard<std::mutex> lock(mtx_);
    return inbox_.recv(buf, cap, out_len, out_conn_id);
  }

  void poll() override {
    // msquic is fully async; events fire on internal threads
  }

  void close() override {
    // Fully no-op: any synchronous msquic teardown call (ConnectionClose,
    // ListenerClose, RegistrationClose, MsQuicClose) races with workers
    // still draining callbacks at this scale and ends in deadlock or
    // glibc double-free. The harness is exiting; the OS reclaims fds and
    // memory at process termination.
  }

  const char* name() const override { return "msquic"; }
  bool supports(bool /*reliable*/) const override { return true; }
  size_t max_payload_bytes(bool reliable) const override {
    return reliable ? 65536 : 1000;
  }
  const char* flush_policy(bool /*reliable*/) const override { return "async_internal"; }
  bool encryption_on() const override { return true; }

  // --- Callback dispatchers (called from msquic internal threads) ---

  QUIC_STATUS on_listener_event(HQUIC /*listener*/, QUIC_LISTENER_EVENT* ev) {
    if (ev->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION) return QUIC_STATUS_SUCCESS;

    HQUIC conn = ev->NEW_CONNECTION.Connection;
    MsQuic->SetCallbackHandler(conn, (void*)connection_cb, this);

    uint32_t id;
    {
      std::lock_guard<std::mutex> lock(mtx_);
      id = next_id_++;
      conn_to_id_[conn] = id;
      id_to_conn_[id] = conn;
    }

    QUIC_STATUS status = MsQuic->ConnectionSetConfiguration(conn, config_);
    if (QUIC_FAILED(status)) return status;

    return QUIC_STATUS_SUCCESS;
  }

  QUIC_STATUS on_connection_event(HQUIC conn, QUIC_CONNECTION_EVENT* ev) {
    switch (ev->Type) {
      case QUIC_CONNECTION_EVENT_CONNECTED: {
        uint32_t now = connected_now_.fetch_add(1) + 1;
        uint32_t peak = connected_peak_.load();
        while (now > peak &&
               !connected_peak_.compare_exchange_weak(peak, now)) {
        }
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = conn_to_id_.find(conn);
        if (it != conn_to_id_.end()) connected_ids_.insert(it->second);
        break;
      }

      case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED: {
        HQUIC stream = ev->PEER_STREAM_STARTED.Stream;
        uint32_t cid = 0;
        {
          std::lock_guard<std::mutex> lock(mtx_);
          auto it = conn_to_id_.find(conn);
          if (it != conn_to_id_.end()) cid = it->second;
        }
        auto* sctx = new StreamCtx(this, cid, conn, false);
        MsQuic->SetCallbackHandler(stream, (void*)stream_cb, sctx);
        return QUIC_STATUS_SUCCESS;
      }

      case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED: {
        uint32_t cid = 0;
        {
          std::lock_guard<std::mutex> lock(mtx_);
          auto it = conn_to_id_.find(conn);
          if (it != conn_to_id_.end()) cid = it->second;
        }
        const auto* buf = ev->DATAGRAM_RECEIVED.Buffer;
        {
          std::lock_guard<std::mutex> lock(mtx_);
          inbox_.enqueue(cid, buf->Buffer, buf->Length);
        }
        break;
      }

      case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED: {
        if (QUIC_DATAGRAM_SEND_STATE_IS_FINAL(
                ev->DATAGRAM_SEND_STATE_CHANGED.State)) {
          auto* ctx = static_cast<SendCtx*>(
              ev->DATAGRAM_SEND_STATE_CHANGED.ClientContext);
          delete ctx;
          ev->DATAGRAM_SEND_STATE_CHANGED.ClientContext = nullptr;
        }
        break;
      }

      case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
        shutdown_by_transport_.fetch_add(1);
        break;
      case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        shutdown_by_peer_.fetch_add(1);
        break;

      case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = conn_to_id_.find(conn);
        if (it != conn_to_id_.end()) {
          uint32_t id = it->second;
          if (connected_ids_.erase(id)) connected_now_.fetch_sub(1);
          id_to_conn_.erase(id);
          reliable_stream_by_conn_.erase(conn);
          conn_to_id_.erase(it);
        }
        break;
      }

      default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
  }

  QUIC_STATUS on_stream_event(HQUIC stream, StreamCtx* sctx,
                              QUIC_STREAM_EVENT* ev) {
    switch (ev->Type) {
      case QUIC_STREAM_EVENT_RECEIVE: {
        for (uint32_t i = 0; i < ev->RECEIVE.BufferCount; ++i) {
          const uint8_t* p = ev->RECEIVE.Buffers[i].Buffer;
          uint32_t blen = ev->RECEIVE.Buffers[i].Length;
          sctx->recv_buf.insert(sctx->recv_buf.end(), p, p + blen);
        }
        drain_frames(sctx);
        break;
      }

      case QUIC_STREAM_EVENT_SEND_COMPLETE: {
        auto* ctx = static_cast<SendCtx*>(ev->SEND_COMPLETE.ClientContext);
        delete ctx;
        break;
      }

      case QUIC_STREAM_EVENT_PEER_SEND_SHUTDOWN:
        MsQuic->StreamShutdown(stream, QUIC_STREAM_SHUTDOWN_FLAG_GRACEFUL, 0);
        break;

      case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        if (sctx->outbound) {
          std::lock_guard<std::mutex> lock(mtx_);
          auto it = reliable_stream_by_conn_.find(sctx->conn);
          if (it != reliable_stream_by_conn_.end() && it->second == stream) {
            reliable_stream_by_conn_.erase(it);
          }
        }
        MsQuic->StreamClose(stream);
        delete sctx;
        return QUIC_STATUS_SUCCESS;

      default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
  }

  void enqueue_msg(uint32_t conn_id, const uint8_t* data, size_t len) {
    std::lock_guard<std::mutex> lock(mtx_);
    inbox_.enqueue(conn_id, data, len);
  }

 private:
  HQUIC reliable_stream(HQUIC conn) {
    {
      std::lock_guard<std::mutex> lock(mtx_);
      auto sit = reliable_stream_by_conn_.find(conn);
      if (sit != reliable_stream_by_conn_.end()) return sit->second;
    }

    uint32_t cid = 0;
    {
      std::lock_guard<std::mutex> lock(mtx_);
      auto it = conn_to_id_.find(conn);
      if (it == conn_to_id_.end()) return nullptr;
      cid = it->second;
    }

    HQUIC stream = nullptr;
    auto* sctx = new StreamCtx(this, cid, conn, true);
    QUIC_STATUS status;
    status = MsQuic->StreamOpen(conn, QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL, stream_cb, sctx, &stream);
    if (QUIC_FAILED(status)) { delete sctx; return nullptr; }
    status = MsQuic->StreamStart(stream, QUIC_STREAM_START_FLAG_IMMEDIATE);
    if (QUIC_FAILED(status)) { MsQuic->StreamClose(stream); delete sctx; return nullptr; }

    {
      std::lock_guard<std::mutex> lock(mtx_);
      reliable_stream_by_conn_[conn] = stream;
    }
    return stream;
  }

  int send_stream(HQUIC conn, const void* data, size_t len) {
    HQUIC stream = reliable_stream(conn);
    if (!stream) return -1;

    // Length-prefix (4 bytes big-endian) + payload
    auto* ctx = new SendCtx();
    ctx->data.resize(4 + len);
    uint32_t nlen = htonl(static_cast<uint32_t>(len));
    std::memcpy(ctx->data.data(), &nlen, 4);
    std::memcpy(ctx->data.data() + 4, data, len);
    ctx->buf.Buffer = ctx->data.data();
    ctx->buf.Length = static_cast<uint32_t>(ctx->data.size());

    QUIC_STATUS status = MsQuic->StreamSend(stream, &ctx->buf, 1, QUIC_SEND_FLAG_NONE, ctx);
    if (QUIC_FAILED(status)) { delete ctx; return -1; }
    return 0;
  }

  int send_datagram(HQUIC conn, const void* data, size_t len) {
    auto* ctx = new SendCtx();
    ctx->data.assign(static_cast<const uint8_t*>(data),
                     static_cast<const uint8_t*>(data) + len);
    ctx->buf.Buffer = ctx->data.data();
    ctx->buf.Length = static_cast<uint32_t>(len);

    if (QUIC_FAILED(MsQuic->DatagramSend(conn, &ctx->buf, 1, QUIC_SEND_FLAG_NONE, ctx))) {
      delete ctx;
      return -1;
    }
    return 0;
  }

  void drain_frames(StreamCtx* sctx) {
    auto& rb = sctx->recv_buf;
    auto available = [&]() -> size_t { return rb.size() - sctx->recv_offset; };
    while (true) {
      if (!sctx->have_len) {
        if (available() < 4) break;
        uint32_t nlen;
        std::memcpy(&nlen, rb.data() + sctx->recv_offset, 4);
        sctx->recv_offset += 4;
        sctx->frame_len = ntohl(nlen);
        sctx->have_len = true;
      }
      if (available() < sctx->frame_len) break;
      enqueue_msg(sctx->conn_id, rb.data() + sctx->recv_offset, sctx->frame_len);
      sctx->recv_offset += sctx->frame_len;
      sctx->have_len = false;
    }
    if (sctx->recv_offset == rb.size()) {
      rb.clear();
      sctx->recv_offset = 0;
    } else if (sctx->recv_offset > 4096 && sctx->recv_offset * 2 >= rb.size()) {
      rb.erase(rb.begin(), rb.begin() + static_cast<std::ptrdiff_t>(sctx->recv_offset));
      sctx->recv_offset = 0;
    }
  }

  // Static callbacks route to adapter instance via Context pointer
  static QUIC_STATUS QUIC_API listener_cb(HQUIC listener, void* ctx,
                                          QUIC_LISTENER_EVENT* ev) {
    return static_cast<MsquicAdapter*>(ctx)->on_listener_event(listener, ev);
  }

  static QUIC_STATUS QUIC_API connection_cb(HQUIC conn, void* ctx,
                                            QUIC_CONNECTION_EVENT* ev) {
    return static_cast<MsquicAdapter*>(ctx)->on_connection_event(conn, ev);
  }

  static QUIC_STATUS QUIC_API stream_cb(HQUIC stream, void* ctx,
                                        QUIC_STREAM_EVENT* ev) {
    auto* sctx = static_cast<StreamCtx*>(ctx);
    return sctx->adapter->on_stream_event(stream, sctx, ev);
  }

  bool is_server_ = false;
  HQUIC reg_ = nullptr;
  HQUIC config_ = nullptr;
  HQUIC listener_ = nullptr;

  std::mutex mtx_;
  std::unordered_map<uint32_t, HQUIC> id_to_conn_;
  std::unordered_map<HQUIC, uint32_t> conn_to_id_;
  std::unordered_map<HQUIC, HQUIC> reliable_stream_by_conn_;
  std::unordered_set<uint32_t> connected_ids_;
  uint32_t next_id_ = 1;

  // 接続イベントの累計(connection_stats() で読み取る)
  std::atomic<uint32_t> connected_peak_{0};
  std::atomic<uint32_t> connected_now_{0};
  std::atomic<uint32_t> shutdown_by_transport_{0};
  std::atomic<uint32_t> shutdown_by_peer_{0};

  rudp_bench::ReusableInboundQueue inbox_;

 public:
  rudp_bench::ConnectionStats connection_stats() const override {
    return {connected_peak_.load(), shutdown_by_transport_.load(),
            shutdown_by_peer_.load()};
  }
};

}  // namespace

namespace rudp_bench {
void register_msquic_adapter() {
  register_adapter("msquic", []() { return std::make_unique<MsquicAdapter>(); });
}
}  // namespace rudp_bench
