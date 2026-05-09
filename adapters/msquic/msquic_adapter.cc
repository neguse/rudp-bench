#include "harness/adapter.h"
#include "harness/adapter_registry.h"

#include <msquic.h>

#include <arpa/inet.h>
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

void ensure_msquic_init() {
  static std::once_flag flag;
  std::call_once(flag, []() {
    if (QUIC_FAILED(MsQuicOpen2(&MsQuic))) std::abort();

    // Generate self-signed cert for benchmark use
    int rc = std::system(
        "openssl req -x509 -newkey rsa:2048 -nodes -days 365 "
        "-keyout /tmp/msquic_key.pem -out /tmp/msquic_cert.pem "
        "-subj '/CN=rudp-bench' 2>/dev/null");
    if (rc != 0) std::abort();

    std::atexit([]() { MsQuicClose(MsQuic); });
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
  std::vector<uint8_t> recv_buf;
  uint32_t frame_len = 0;
  bool have_len = false;
};

class MsquicAdapter : public rudp_bench::Adapter {
 public:
  MsquicAdapter() { ensure_msquic_init(); }
  ~MsquicAdapter() override { close(); }

  void server_listen(uint16_t port) override {
    is_server_ = true;

    QUIC_REGISTRATION_CONFIG reg_cfg = {"rudp-bench-srv", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
    QUIC_STATUS status;
    status = MsQuic->RegistrationOpen(&reg_cfg, &reg_);
    if (QUIC_FAILED(status)) std::abort();

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
    if (QUIC_FAILED(status)) std::abort();

    QUIC_CERTIFICATE_FILE cert_file;
    cert_file.CertificateFile = "/tmp/msquic_cert.pem";
    cert_file.PrivateKeyFile = "/tmp/msquic_key.pem";

    QUIC_CREDENTIAL_CONFIG cred_cfg{};
    cred_cfg.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    cred_cfg.CertificateFile = &cert_file;
    cred_cfg.Flags = QUIC_CREDENTIAL_FLAG_NONE;

    status = MsQuic->ConfigurationLoadCredential(config_, &cred_cfg);
    if (QUIC_FAILED(status)) std::abort();

    status = MsQuic->ListenerOpen(reg_, listener_cb, this, &listener_);
    if (QUIC_FAILED(status)) std::abort();

    QUIC_ADDR addr{};
    std::memset(&addr, 0, sizeof(addr));
    QuicAddrSetFamily(&addr, QUIC_ADDRESS_FAMILY_UNSPEC);
    QuicAddrSetPort(&addr, port);

    status = MsQuic->ListenerStart(listener_, &Alpn, 1, &addr);
    if (QUIC_FAILED(status)) std::abort();
  }

  uint32_t client_connect(const char* host, uint16_t port) override {
    if (!reg_) {
      QUIC_REGISTRATION_CONFIG reg_cfg = {"rudp-bench-cli", QUIC_EXECUTION_PROFILE_LOW_LATENCY};
      if (QUIC_FAILED(MsQuic->RegistrationOpen(&reg_cfg, &reg_))) std::abort();

      QUIC_SETTINGS settings{};
      settings.IdleTimeoutMs = 30000;
      settings.IsSet.IdleTimeoutMs = TRUE;
      settings.PeerBidiStreamCount = 16384;
      settings.IsSet.PeerBidiStreamCount = TRUE;
      settings.PeerUnidiStreamCount = 16384;
      settings.IsSet.PeerUnidiStreamCount = TRUE;
      settings.DatagramReceiveEnabled = TRUE;
      settings.IsSet.DatagramReceiveEnabled = TRUE;

      if (QUIC_FAILED(MsQuic->ConfigurationOpen(
              reg_, &Alpn, 1, &settings, sizeof(settings), nullptr, &config_)))
        std::abort();

      QUIC_CREDENTIAL_CONFIG cred_cfg{};
      cred_cfg.Type = QUIC_CREDENTIAL_TYPE_NONE;
      cred_cfg.Flags = QUIC_CREDENTIAL_FLAG_CLIENT |
                        QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;

      if (QUIC_FAILED(MsQuic->ConfigurationLoadCredential(config_, &cred_cfg)))
        std::abort();
    }

    HQUIC conn = nullptr;
    if (QUIC_FAILED(MsQuic->ConnectionOpen(reg_, connection_cb, this, &conn)))
      std::abort();

    uint32_t id;
    {
      std::lock_guard<std::mutex> lock(mtx_);
      id = next_id_++;
      id_to_conn_[id] = conn;
      conn_to_id_[conn] = id;
    }

    if (QUIC_FAILED(MsQuic->ConnectionStart(
            conn, config_, QUIC_ADDRESS_FAMILY_UNSPEC, host, port)))
      std::abort();

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
    if (inbox_.empty()) return 0;
    auto& m = inbox_.front();
    if (m.data.size() > cap) {
      *out_len = m.data.size();
      *out_conn_id = m.conn_id;
      inbox_.pop_front();
      return -1;
    }
    std::memcpy(buf, m.data.data(), m.data.size());
    *out_len = m.data.size();
    *out_conn_id = m.conn_id;
    inbox_.pop_front();
    return 1;
  }

  void poll() override {
    // msquic is fully async; events fire on internal threads
  }

  void close() override {
    if (listener_) {
      MsQuic->ListenerClose(listener_);
      listener_ = nullptr;
    }

    std::vector<HQUIC> to_close;
    {
      std::lock_guard<std::mutex> lock(mtx_);
      for (auto& [id, conn] : id_to_conn_) to_close.push_back(conn);
      id_to_conn_.clear();
      conn_to_id_.clear();
      connected_ids_.clear();
      inbox_.clear();
    }
    for (auto conn : to_close) {
      MsQuic->ConnectionShutdown(conn, QUIC_CONNECTION_SHUTDOWN_FLAG_NONE, 0);
      MsQuic->ConnectionClose(conn);
    }

    if (config_) { MsQuic->ConfigurationClose(config_); config_ = nullptr; }
    if (reg_) { MsQuic->RegistrationClose(reg_); reg_ = nullptr; }
  }

  const char* name() const override { return "msquic"; }
  bool supports(bool /*reliable*/) const override { return true; }
  size_t max_payload_bytes(bool reliable) const override {
    return reliable ? 65536 : 1000;
  }
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
        auto* sctx = new StreamCtx{this, cid, {}, 0, false};
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
        InboundMsg m;
        m.conn_id = cid;
        m.data.assign(buf->Buffer, buf->Buffer + buf->Length);
        {
          std::lock_guard<std::mutex> lock(mtx_);
          inbox_.push_back(std::move(m));
        }
        break;
      }

      case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
      case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        break;

      case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE: {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = conn_to_id_.find(conn);
        if (it != conn_to_id_.end()) {
          uint32_t id = it->second;
          connected_ids_.erase(id);
          id_to_conn_.erase(id);
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
        MsQuic->StreamClose(stream);
        delete sctx;
        return QUIC_STATUS_SUCCESS;

      default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
  }

  void enqueue_msg(uint32_t conn_id, const uint8_t* data, size_t len) {
    InboundMsg m;
    m.conn_id = conn_id;
    m.data.assign(data, data + len);
    std::lock_guard<std::mutex> lock(mtx_);
    inbox_.push_back(std::move(m));
  }

 private:
  struct InboundMsg {
    uint32_t conn_id;
    std::vector<uint8_t> data;
  };

  int send_stream(HQUIC conn, const void* data, size_t len) {
    HQUIC stream = nullptr;
    auto* sctx = new StreamCtx{this, 0, {}, 0, false};
    QUIC_STATUS status;
    status = MsQuic->StreamOpen(conn, QUIC_STREAM_OPEN_FLAG_UNIDIRECTIONAL, stream_cb, sctx, &stream);
    if (QUIC_FAILED(status)) { delete sctx; return -1; }
    status = MsQuic->StreamStart(stream, QUIC_STREAM_START_FLAG_IMMEDIATE);
    if (QUIC_FAILED(status)) { MsQuic->StreamClose(stream); delete sctx; return -1; }

    // Length-prefix (4 bytes big-endian) + payload
    auto* ctx = new SendCtx();
    ctx->data.resize(4 + len);
    uint32_t nlen = htonl(static_cast<uint32_t>(len));
    std::memcpy(ctx->data.data(), &nlen, 4);
    std::memcpy(ctx->data.data() + 4, data, len);
    ctx->buf.Buffer = ctx->data.data();
    ctx->buf.Length = static_cast<uint32_t>(ctx->data.size());

    status = MsQuic->StreamSend(stream, &ctx->buf, 1, QUIC_SEND_FLAG_FIN, ctx);
    if (QUIC_FAILED(status)) { delete ctx; MsQuic->StreamClose(stream); delete sctx; return -1; }
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
    while (true) {
      if (!sctx->have_len) {
        if (rb.size() < 4) return;
        uint32_t nlen;
        std::memcpy(&nlen, rb.data(), 4);
        sctx->frame_len = ntohl(nlen);
        sctx->have_len = true;
        rb.erase(rb.begin(), rb.begin() + 4);
      }
      if (rb.size() < sctx->frame_len) return;
      enqueue_msg(sctx->conn_id, rb.data(), sctx->frame_len);
      rb.erase(rb.begin(), rb.begin() + sctx->frame_len);
      sctx->have_len = false;
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
  std::unordered_set<uint32_t> connected_ids_;
  uint32_t next_id_ = 1;

  std::deque<InboundMsg> inbox_;
};

}  // namespace

namespace rudp_bench {
void register_msquic_adapter() {
  register_adapter("msquic", []() { return std::make_unique<MsquicAdapter>(); });
}
}  // namespace rudp_bench
