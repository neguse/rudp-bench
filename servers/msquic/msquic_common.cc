#include "msquic_common.h"

#include <arpa/inet.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
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
  settings->IdleTimeoutMs = 30000;
  settings->IsSet.IdleTimeoutMs = TRUE;
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
      "{\"knob\":\"QUIC_SETTINGS.CongestionControlAlgorithm\","
      "\"value\":\"QUIC_CONGESTION_CONTROL_ALGORITHM_BBR\","
      "\"upstream_ref\":\"MsQuic QUIC_SETTINGS.CongestionControlAlgorithm; "
      "third_party/msquic/src/inc/msquic.h\"},"
      "{\"knob\":\"QUIC_SETTINGS.DatagramReceiveEnabled\","
      "\"value\":\"TRUE\","
      "\"upstream_ref\":\"MsQuic QUIC_SETTINGS.DatagramReceiveEnabled and "
      "RFC 9221 QUIC Datagrams\"}"
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

int send_datagram_payload(HQUIC conn, const void *data, size_t len) {
  if (len > UINT32_MAX) {
    return -1;
  }
  auto *ctx = new SendCtx();
  ctx->data.assign(static_cast<const uint8_t *>(data),
                   static_cast<const uint8_t *>(data) + len);
  ctx->buffer.Buffer = ctx->data.data();
  ctx->buffer.Length = static_cast<uint32_t>(ctx->data.size());
  const QUIC_STATUS status =
      MsQuic->DatagramSend(conn, &ctx->buffer, 1, QUIC_SEND_FLAG_NONE, ctx);
  if (QUIC_FAILED(status)) {
    delete ctx;
    return -1;
  }
  return 0;
}

int send_stream_frame(HQUIC stream, const void *data, size_t len) {
  if (len > UINT32_MAX) {
    return -1;
  }
  auto *ctx = new SendCtx();
  ctx->data.resize(sizeof(uint32_t) + len);
  const uint32_t nlen = htonl(static_cast<uint32_t>(len));
  std::memcpy(ctx->data.data(), &nlen, sizeof(nlen));
  std::memcpy(ctx->data.data() + sizeof(nlen), data, len);
  ctx->buffer.Buffer = ctx->data.data();
  ctx->buffer.Length = static_cast<uint32_t>(ctx->data.size());
  const QUIC_STATUS status =
      MsQuic->StreamSend(stream, &ctx->buffer, 1, QUIC_SEND_FLAG_NONE, ctx);
  if (QUIC_FAILED(status)) {
    delete ctx;
    return -1;
  }
  return 0;
}

void handle_datagram_send_state(QUIC_CONNECTION_EVENT *event) {
  if (event->Type != QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED ||
      !QUIC_DATAGRAM_SEND_STATE_IS_FINAL(
          event->DATAGRAM_SEND_STATE_CHANGED.State)) {
    return;
  }
  auto *ctx =
      static_cast<SendCtx *>(event->DATAGRAM_SEND_STATE_CHANGED.ClientContext);
  delete ctx;
  event->DATAGRAM_SEND_STATE_CHANGED.ClientContext = nullptr;
}

void handle_stream_send_complete(QUIC_STREAM_EVENT *event) {
  if (event->Type != QUIC_STREAM_EVENT_SEND_COMPLETE) {
    return;
  }
  auto *ctx = static_cast<SendCtx *>(event->SEND_COMPLETE.ClientContext);
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
