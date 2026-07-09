#include "msquic_common.h"

#include <arpa/inet.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <new>
#include <string>

namespace rudp_bench_msquic {

const QUIC_API_TABLE *MsQuic = nullptr;

namespace {

uint8_t kAlpnBytes[] = {'r', 'u', 'd', 'p', '-', 'b', 'n', 'c', 'h'};
const QUIC_BUFFER kAlpn = {sizeof(kAlpnBytes), kAlpnBytes};

CertPaths g_cert_paths = {nullptr, nullptr};
std::string g_cert_path_storage;
std::string g_key_path_storage;

}  // namespace

void ensure_msquic() {
  static std::once_flag flag;
  std::call_once(flag, []() {
    const QUIC_STATUS status = MsQuicOpen2(&MsQuic);
    if (QUIC_FAILED(status)) {
      std::fprintf(stderr, "MsQuicOpen2 failed status=0x%x\n",
                   static_cast<unsigned>(status));
      std::abort();
    }
  });
}

const QUIC_BUFFER *alpn() { return &kAlpn; }

void apply_common_settings(QUIC_SETTINGS *settings) {
  std::memset(settings, 0, sizeof(*settings));
  // tune-to-plateau(全て upstream 公式ノブ。--describe の tuning に開示):
  // - Idle/Disconnect timeout: DisconnectTimeoutMs 既定 16s は「最古の未 ACK
  //   が 16s 未確認で QUIC_STATUS_CONNECTION_TIMEOUT」(loss_detection.c:
  //   1853-1866)。過負荷で ACK が滞ると transport shutdown → client crash
  //   (docs/ledger.md #17)の主経路。60s へ延長し、KeepAlive で片方向
  //   無通信時の idle timeout も防ぐ
  // - SendBufferingEnabled=FALSE: 既定 TRUE は StreamSend データを内部
  //   バッファへ copy する(stream_send.c:264,468,661)。SendBuf を
  //   SEND_COMPLETE まで保持する本実装では不要な copy
  // - StreamRecvWindowDefault: 既定 64KB → 1MB(md fanout の flow control)
  settings->IdleTimeoutMs = 60000;
  settings->IsSet.IdleTimeoutMs = TRUE;
  settings->DisconnectTimeoutMs = 60000;
  settings->IsSet.DisconnectTimeoutMs = TRUE;
  settings->KeepAliveIntervalMs = 5000;
  settings->IsSet.KeepAliveIntervalMs = TRUE;
  settings->HandshakeIdleTimeoutMs = 30000;
  settings->IsSet.HandshakeIdleTimeoutMs = TRUE;
  settings->SendBufferingEnabled = FALSE;
  settings->IsSet.SendBufferingEnabled = TRUE;
  settings->StreamRecvWindowDefault = 1024 * 1024;
  settings->IsSet.StreamRecvWindowDefault = TRUE;
  settings->PeerBidiStreamCount = 16384;
  settings->IsSet.PeerBidiStreamCount = TRUE;
  settings->DatagramReceiveEnabled = TRUE;
  settings->IsSet.DatagramReceiveEnabled = TRUE;
  settings->CongestionControlAlgorithm =
      QUIC_CONGESTION_CONTROL_ALGORITHM_BBR;
  settings->IsSet.CongestionControlAlgorithm = TRUE;
}

const CertPaths &ensure_self_signed_cert() {
  static std::once_flag flag;
  std::call_once(flag, []() {
    char cert_path[128];
    char key_path[128];
    std::snprintf(cert_path, sizeof(cert_path),
                  "/tmp/rudp-bench-msquic-v2-%ld-cert.pem",
                  static_cast<long>(::getpid()));
    std::snprintf(key_path, sizeof(key_path),
                  "/tmp/rudp-bench-msquic-v2-%ld-key.pem",
                  static_cast<long>(::getpid()));
    g_cert_path_storage = cert_path;
    g_key_path_storage = key_path;
    g_cert_paths = {g_cert_path_storage.c_str(), g_key_path_storage.c_str()};

    const std::string cmd =
        "openssl req -x509 -newkey rsa:2048 -nodes -days 365 "
        "-keyout " +
        g_key_path_storage + " -out " + g_cert_path_storage +
        " -subj '/CN=rudp-bench' 2>/dev/null";
    if (std::system(cmd.c_str()) != 0) {
      std::fprintf(stderr, "failed to generate self-signed certificate\n");
      std::abort();
    }

    std::atexit([]() {
      if (!g_cert_path_storage.empty()) {
        ::unlink(g_cert_path_storage.c_str());
      }
      if (!g_key_path_storage.empty()) {
        ::unlink(g_key_path_storage.c_str());
      }
    });
  });
  return g_cert_paths;
}

void print_describe() {
  std::puts(
      "{\"transport\":\"msquic\","
      "\"class_mapping\":{\"loss_tolerant\":\"quic-datagram\","
      "\"must_deliver\":\"quic-stream\"},"
      "\"coalescing\":\"none\","
      "\"cc_algo\":\"bbr\","
      "\"thread_model\":\"internal_worker\","
      "\"encryption\":true,"
      "\"max_payload_bytes\":1000,"
      "\"tuning\":["
      "\"cc=bbr\",\"datagram_receive=on\","
      "\"disconnect_timeout=60s\",\"idle_timeout=60s\",\"keep_alive=5s\","
      "\"send_buffering=off\",\"stream_recv_window=1MB\","
      "\"shared-sendbuf-broadcast\",\"sendbuf-direct-write\","
      "\"atomic-stats/cow-conns\""
      "]}");
}

uint64_t add_ns(uint64_t a, uint64_t b) {
  return UINT64_MAX - a < b ? UINT64_MAX : a + b;
}

bool status_ok(QUIC_STATUS status, const char *what) {
  if (QUIC_SUCCEEDED(status)) {
    return true;
  }
  std::fprintf(stderr, "%s failed status=0x%x\n", what,
               static_cast<unsigned>(status));
  return false;
}

SendBuf *send_buf_new(size_t len, int refs) {
  if (len > UINT32_MAX) {
    return nullptr;
  }
  auto *buf = static_cast<SendBuf *>(std::malloc(sizeof(SendBuf) + len));
  if (buf == nullptr) {
    return nullptr;
  }
  new (&buf->refs) std::atomic<int>(refs);
  buf->len = static_cast<uint32_t>(len);
  return buf;
}

void send_buf_unref(SendBuf *buf) {
  if (buf->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    std::free(buf);
  }
}

int send_datagram_buf(HQUIC conn, SendBuf *buf) {
  auto *ctx = new SendCtx();
  ctx->buf = buf;
  ctx->buffer.Buffer = buf->data();
  ctx->buffer.Length = buf->len;
  const QUIC_STATUS status =
      MsQuic->DatagramSend(conn, &ctx->buffer, 1, QUIC_SEND_FLAG_NONE, ctx);
  if (QUIC_FAILED(status)) {
    delete ctx;
    send_buf_unref(buf);
    return -1;
  }
  return 0;
}

int send_stream_buf(HQUIC stream, SendBuf *buf) {
  auto *ctx = new SendCtx();
  ctx->buf = buf;
  ctx->buffer.Buffer = buf->data();
  ctx->buffer.Length = buf->len;
  const QUIC_STATUS status =
      MsQuic->StreamSend(stream, &ctx->buffer, 1, QUIC_SEND_FLAG_NONE, ctx);
  if (QUIC_FAILED(status)) {
    delete ctx;
    send_buf_unref(buf);
    return -1;
  }
  return 0;
}

int send_datagram_payload(HQUIC conn, const void *data, size_t len) {
  SendBuf *buf = send_buf_new(len, 1);
  if (buf == nullptr) {
    return -1;
  }
  std::memcpy(buf->data(), data, len);
  return send_datagram_buf(conn, buf);
}

int send_stream_frame(HQUIC stream, const void *data, size_t len) {
  SendBuf *buf = send_buf_new(sizeof(uint32_t) + len, 1);
  if (buf == nullptr) {
    return -1;
  }
  const uint32_t nlen = htonl(static_cast<uint32_t>(len));
  std::memcpy(buf->data(), &nlen, sizeof(nlen));
  std::memcpy(buf->data() + sizeof(nlen), data, len);
  return send_stream_buf(stream, buf);
}

void handle_datagram_send_state(QUIC_CONNECTION_EVENT *event) {
  if (event->Type != QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED ||
      !QUIC_DATAGRAM_SEND_STATE_IS_FINAL(
          event->DATAGRAM_SEND_STATE_CHANGED.State)) {
    return;
  }
  auto *ctx =
      static_cast<SendCtx *>(event->DATAGRAM_SEND_STATE_CHANGED.ClientContext);
  send_buf_unref(ctx->buf);
  delete ctx;
  event->DATAGRAM_SEND_STATE_CHANGED.ClientContext = nullptr;
}

void handle_stream_send_complete(QUIC_STREAM_EVENT *event) {
  if (event->Type != QUIC_STREAM_EVENT_SEND_COMPLETE) {
    return;
  }
  auto *ctx = static_cast<SendCtx *>(event->SEND_COMPLETE.ClientContext);
  send_buf_unref(ctx->buf);
  delete ctx;
  event->SEND_COMPLETE.ClientContext = nullptr;
}

void FrameDecoder::append(
    const QUIC_BUFFER *buffers, uint32_t buffer_count,
    const std::function<void(const uint8_t *, size_t)> &on_frame) {
  for (uint32_t i = 0; i < buffer_count; ++i) {
    const uint8_t *p = buffers[i].Buffer;
    bytes_.insert(bytes_.end(), p, p + buffers[i].Length);
  }

  auto available = [&]() -> size_t { return bytes_.size() - offset_; };
  for (;;) {
    if (!have_len_) {
      if (available() < sizeof(uint32_t)) {
        break;
      }
      uint32_t nlen = 0;
      std::memcpy(&nlen, bytes_.data() + offset_, sizeof(nlen));
      offset_ += sizeof(nlen);
      frame_len_ = ntohl(nlen);
      have_len_ = true;
    }
    if (available() < frame_len_) {
      break;
    }
    on_frame(bytes_.data() + offset_, frame_len_);
    offset_ += frame_len_;
    have_len_ = false;
  }

  if (offset_ == bytes_.size()) {
    bytes_.clear();
    offset_ = 0;
  } else if (offset_ > 4096 && offset_ * 2 >= bytes_.size()) {
    bytes_.erase(bytes_.begin(),
                 bytes_.begin() + static_cast<std::ptrdiff_t>(offset_));
    offset_ = 0;
  }
}

}  // namespace rudp_bench_msquic
