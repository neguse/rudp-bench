#pragma once

namespace rudp_bench {

struct SocketBufferResult {
  int requested_rcv = 0;
  int requested_snd = 0;
  int actual_rcv = 0;
  int actual_snd = 0;
  int effective_rcv = 0;
  int effective_snd = 0;
  bool rcv_set_ok = false;
  bool snd_set_ok = false;
  bool rcv_get_ok = false;
  bool snd_get_ok = false;
  bool rcv_clamped = false;
  bool snd_clamped = false;
};

SocketBufferResult tune_udp_socket_buffers(int fd, int rcv_bytes, int snd_bytes,
                                           const char* adapter,
                                           const char* socket_role);

}  // namespace rudp_bench
