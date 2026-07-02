#include "harness/socket_buffer.h"

#include <sys/socket.h>

#include <cerrno>
#include <cstdio>

namespace rudp_bench {
namespace {

int effective_socket_buffer_bytes(int actual) {
#if defined(__linux__)
  return actual > 0 ? actual / 2 : actual;
#else
  return actual;
#endif
}

bool get_socket_buffer(int fd, int optname, int* out_value) {
  socklen_t len = sizeof(*out_value);
  return ::getsockopt(fd, SOL_SOCKET, optname, out_value, &len) == 0;
}

void log_socket_buffer(const char* adapter, const char* socket_role, int fd,
                       const char* opt, int requested, int actual,
                       int effective, bool set_ok, int set_errno,
                       bool get_ok, int get_errno, bool clamped) {
  std::fprintf(stderr,
               "socket_buffer adapter=%s socket=%s fd=%d opt=%s "
               "requested=%d actual=%d effective=%d set_ok=%d set_errno=%d "
               "get_ok=%d get_errno=%d clamped=%d\n",
               adapter ? adapter : "unknown",
               socket_role ? socket_role : "unknown",
               fd, opt, requested, actual, effective, set_ok ? 1 : 0,
               set_ok ? 0 : set_errno, get_ok ? 1 : 0, get_ok ? 0 : get_errno,
               clamped ? 1 : 0);
}

}  // namespace

SocketBufferResult tune_udp_socket_buffers(int fd, int rcv_bytes, int snd_bytes,
                                           const char* adapter,
                                           const char* socket_role) {
  SocketBufferResult r;
  r.requested_rcv = rcv_bytes;
  r.requested_snd = snd_bytes;

  errno = 0;
  r.rcv_set_ok =
      ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv_bytes, sizeof(rcv_bytes)) == 0;
  int rcv_set_errno = errno;

  errno = 0;
  r.snd_set_ok =
      ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &snd_bytes, sizeof(snd_bytes)) == 0;
  int snd_set_errno = errno;

  errno = 0;
  r.rcv_get_ok = get_socket_buffer(fd, SO_RCVBUF, &r.actual_rcv);
  int rcv_get_errno = errno;

  errno = 0;
  r.snd_get_ok = get_socket_buffer(fd, SO_SNDBUF, &r.actual_snd);
  int snd_get_errno = errno;

  r.effective_rcv = r.rcv_get_ok ? effective_socket_buffer_bytes(r.actual_rcv) : 0;
  r.effective_snd = r.snd_get_ok ? effective_socket_buffer_bytes(r.actual_snd) : 0;
  r.rcv_clamped = r.rcv_get_ok && r.effective_rcv < r.requested_rcv;
  r.snd_clamped = r.snd_get_ok && r.effective_snd < r.requested_snd;

  log_socket_buffer(adapter, socket_role, fd, "SO_RCVBUF", r.requested_rcv,
                    r.actual_rcv, r.effective_rcv, r.rcv_set_ok,
                    rcv_set_errno, r.rcv_get_ok, rcv_get_errno,
                    r.rcv_clamped);
  log_socket_buffer(adapter, socket_role, fd, "SO_SNDBUF", r.requested_snd,
                    r.actual_snd, r.effective_snd, r.snd_set_ok,
                    snd_set_errno, r.snd_get_ok, snd_get_errno,
                    r.snd_clamped);
  return r;
}

}  // namespace rudp_bench
