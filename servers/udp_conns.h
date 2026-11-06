// 素の UDP の上に載る層(raw_udp、KCP)が共有する、接続の表。
// server は 1 つの socket で受け、送信元アドレスごとに接続の番号を振る。
// client は接続ごとに socket を作り、server へ connect(2) する。
#ifndef UDP_CONNS_H
#define UDP_CONNS_H

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

#define UDP_CONNS_RECV_BUDGET 512
#define UDP_CONNS_MAX_DATAGRAM 2048

typedef struct {
  bool server;
  int n;      // server: 接続の上限、client: 接続数
  int known;  // server: 見えた送信元の数
  int n_fds;
  struct pollfd *pfds;
  struct sockaddr_in *peers;
} udp_conns;

// 届いた datagram ごとに呼ぶ。is_new は server で初めて見えた送信元のとき true。
typedef void (*udp_conns_fn)(void *ctx, int conn, bool is_new,
                             const unsigned char *data, size_t len);

static inline void udp_conns_close(udp_conns *u) {
  for (int i = 0; u->pfds != NULL && i < u->n_fds; ++i) {
    if (u->pfds[i].fd >= 0) {
      close(u->pfds[i].fd);
    }
  }
  free(u->pfds);
  free(u->peers);
}

static inline int udp_conns_init(udp_conns *u, bool server, int n) {
  *u = (udp_conns){.server = server, .n = n, .n_fds = server ? 1 : n};
  u->pfds = calloc((size_t)u->n_fds, sizeof(*u->pfds));
  u->peers = calloc((size_t)n, sizeof(*u->peers));
  if (u->pfds == NULL || u->peers == NULL) {
    udp_conns_close(u);
    return -1;
  }
  for (int i = 0; i < u->n_fds; ++i) {
    u->pfds[i] = (struct pollfd){.fd = -1, .events = POLLIN};
  }
  return 0;
}

static inline int udp_conns_listen(udp_conns *u, uint16_t port,
                                   int max_conns) {
  if (udp_conns_init(u, true, max_conns) != 0) {
    return -1;
  }
  const struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(port),
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  u->pfds[0].fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
  if (u->pfds[0].fd < 0 ||
      bind(u->pfds[0].fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
    udp_conns_close(u);
    return -1;
  }
  return 0;
}

static inline int udp_conns_connect(udp_conns *u, const char *host,
                                    uint16_t port, int conns) {
  if (udp_conns_init(u, false, conns) != 0) {
    return -1;
  }
  struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port)};
  bool ok = inet_pton(AF_INET, host, &addr.sin_addr) == 1;
  for (int i = 0; ok && i < conns; ++i) {
    u->pfds[i].fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    ok = u->pfds[i].fd >= 0 &&
         connect(u->pfds[i].fd, (const struct sockaddr *)&addr,
                 sizeof(addr)) == 0;
  }
  if (!ok) {
    udp_conns_close(u);
    return -1;
  }
  return 0;
}

static inline int udp_conns_send(const udp_conns *u, int conn,
                                 const void *data, size_t len) {
  const ssize_t n =
      u->server ? sendto(u->pfds[0].fd, data, len, 0,
                         (const struct sockaddr *)&u->peers[conn],
                         sizeof(u->peers[conn]))
                : send(u->pfds[conn].fd, data, len, 0);
  return n == (ssize_t)len ? 0 : -1;
}

static inline int udp_conns_lookup(udp_conns *u, const struct sockaddr_in *from,
                                   bool *is_new) {
  *is_new = false;
  for (int i = 0; i < u->known; ++i) {
    if (u->peers[i].sin_port == from->sin_port &&
        u->peers[i].sin_addr.s_addr == from->sin_addr.s_addr) {
      return i;
    }
  }
  if (u->known == u->n) {
    return -1;
  }
  u->peers[u->known] = *from;
  *is_new = true;
  return u->known++;
}

// 最大 timeout_ms 待ち、届いた datagram を fn に渡す。0 = 正常、-1 = 失敗。
static inline int udp_conns_poll(udp_conns *u, int timeout_ms, udp_conns_fn fn,
                                 void *ctx) {
  if (poll(u->pfds, (nfds_t)u->n_fds, timeout_ms) < 0) {
    return errno == EINTR ? 0 : -1;
  }
  unsigned char buf[UDP_CONNS_MAX_DATAGRAM];
  for (int i = 0; i < u->n_fds; ++i) {
    if (u->pfds[i].revents == 0) {
      continue;
    }
    for (int k = 0; k < UDP_CONNS_RECV_BUDGET; ++k) {
      struct sockaddr_in from;
      socklen_t from_len = sizeof(from);
      const ssize_t n = recvfrom(u->pfds[i].fd, buf, sizeof(buf), 0,
                                 (struct sockaddr *)&from, &from_len);
      if (n < 0) {
        // ECONNREFUSED は server がまだ居ないときの ICMP で、無視してよい
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ECONNREFUSED) {
          break;
        }
        return -1;
      }
      bool is_new = false;
      const int conn = u->server ? udp_conns_lookup(u, &from, &is_new) : i;
      if (conn >= 0) {
        fn(ctx, conn, is_new, buf, (size_t)n);
      }
    }
  }
  return 0;
}

#endif  // UDP_CONNS_H
