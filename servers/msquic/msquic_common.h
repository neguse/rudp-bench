#ifndef RUDP_BENCH_MSQUIC_COMMON_H
#define RUDP_BENCH_MSQUIC_COMMON_H

#include <msquic.h>

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

struct SendCtx {
  std::vector<uint8_t> data;
  QUIC_BUFFER buffer;
};

int send_datagram_payload(HQUIC conn, const void *data, size_t len);
int send_stream_frame(HQUIC stream, const void *data, size_t len);
void handle_datagram_send_state(QUIC_CONNECTION_EVENT *event);
void handle_stream_send_complete(QUIC_STREAM_EVENT *event);

class FrameDecoder {
 public:
  void append(const QUIC_BUFFER *buffers, uint32_t buffer_count,
              const std::function<void(const uint8_t *, size_t)> &on_frame);

 private:
  std::vector<uint8_t> bytes_;
  size_t offset_ = 0;
  uint32_t frame_len_ = 0;
  bool have_len_ = false;
};

}  // namespace rudp_bench_msquic

#endif  // RUDP_BENCH_MSQUIC_COMMON_H
