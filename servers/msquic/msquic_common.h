#ifndef RUDP_BENCH_MSQUIC_COMMON_H
#define RUDP_BENCH_MSQUIC_COMMON_H

#include <msquic.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace rudp_bench_msquic {

constexpr size_t kMaxPayloadBytes = 1000u;

extern const QUIC_API_TABLE *MsQuic;

struct CertPaths {
  const char *cert;
  const char *key;
};

void ensure_msquic();
const QUIC_BUFFER *alpn();
void apply_common_settings(QUIC_SETTINGS *settings);
const CertPaths &ensure_self_signed_cert();
void print_describe();

uint64_t add_ns(uint64_t a, uint64_t b);
bool status_ok(QUIC_STATUS status, const char *what);

// 送信 frame バイト列(datagram: payload / stream: 4B 長框 + payload)。
// refcount で複数送信間の共有を許す: broadcast fanout では同一 frame を
// N 宛先に送るため、copy とヒープ確保を 1 回にできる。
// unref は msquic worker スレッドの完了イベントから呼ばれるため atomic。
struct SendBuf {
  std::atomic<int> refs;
  uint32_t len;
  uint8_t *data() { return reinterpret_cast<uint8_t *>(this + 1); }
};

SendBuf *send_buf_new(size_t len, int refs);  // data は未初期化
void send_buf_unref(SendBuf *buf);

// 宛先ごとの送信 ctx。QUIC_BUFFER 配列は送信完了まで有効である必要が
// あるため(msquic 契約)、共有 SendBuf を指す QUIC_BUFFER を宛先ごとに持つ。
struct SendCtx {
  SendBuf *buf;
  QUIC_BUFFER buffer;
};

// いずれも SendBuf の参照を 1 消費する(失敗時は即 unref)。
int send_datagram_buf(HQUIC conn, SendBuf *buf);
int send_stream_buf(HQUIC stream, SendBuf *buf);

// echo 用: data を copy した refs=1 の SendBuf を作って送る。
int send_datagram_payload(HQUIC conn, const void *data, size_t len);
int send_stream_frame(HQUIC stream, const void *data, size_t len);

void handle_datagram_send_state(QUIC_CONNECTION_EVENT *event);
void handle_stream_send_complete(QUIC_STREAM_EVENT *event);

class FrameDecoder {
 public:
  // false means an impossible frame length was observed and buffered stream
  // bytes were discarded; callers count it as invalid_payload.
  bool append(const QUIC_BUFFER *buffers, uint32_t buffer_count,
              const std::function<void(const uint8_t *, size_t)> &on_frame);

 private:
  std::vector<uint8_t> bytes_;
  size_t offset_ = 0;
  uint32_t frame_len_ = 0;
  bool have_len_ = false;
};

}  // namespace rudp_bench_msquic

#endif  // RUDP_BENCH_MSQUIC_COMMON_H
