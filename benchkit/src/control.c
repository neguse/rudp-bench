#include "benchkit.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct bk_control {
  int fd;
  // poll_schedule 用の行バッファ(部分受信を跨いで保持する)
  char rbuf[1024];
  size_t rlen;
};

static int fill_sockaddr(const char *sock_path, struct sockaddr_un *addr,
                         socklen_t *addr_len) {
  memset(addr, 0, sizeof(*addr));
  addr->sun_family = AF_UNIX;

  const size_t path_len = strlen(sock_path);
  if (sock_path[0] == '@') {
    if (path_len <= 1u || path_len > sizeof(addr->sun_path)) {
      return -1;
    }
    addr->sun_path[0] = '\0';
    memcpy(addr->sun_path + 1, sock_path + 1, path_len - 1u);
    *addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len);
    return 0;
  }

  if (path_len >= sizeof(addr->sun_path)) {
    return -1;
  }
  memcpy(addr->sun_path, sock_path, path_len + 1u);
  *addr_len =
      (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1u);
  return 0;
}

static bk_control *control_from_fd(int fd) {
  bk_control *c = (bk_control *)calloc(1, sizeof(*c));
  if (c == NULL) {
    close(fd);
    return NULL;
  }
  c->fd = fd;
  return c;
}

static int write_all(int fd, const char *buf, size_t len) {
  size_t off = 0;
  while (off < len) {
    ssize_t n = write(fd, buf + off, len - off);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (n == 0) {
      return -1;
    }
    off += (size_t)n;
  }
  return 0;
}

// rbuf から1行取り出す。1=行あり / 0=まだ / -1=行が cap 超過。
static int extract_line(bk_control *c, char *line, size_t cap) {
  for (size_t i = 0; i < c->rlen; ++i) {
    if (c->rbuf[i] == '\n') {
      if (i >= cap) {
        return -1;
      }
      memcpy(line, c->rbuf, i);
      line[i] = '\0';
      memmove(c->rbuf, c->rbuf + i + 1u, c->rlen - i - 1u);
      c->rlen -= i + 1u;
      return 1;
    }
  }
  return 0;
}

static bool json_has_type(const char *line, const char *type) {
  const char *type_key = strstr(line, "\"type\"");
  if (type_key == NULL) {
    return false;
  }
  const char *p = strstr(type_key, type);
  return p != NULL;
}

static int json_get_u64(const char *line, const char *key, uint64_t *out) {
  const char *p = strstr(line, key);
  if (p == NULL) {
    return -1;
  }
  p += strlen(key);
  while (*p != '\0' && isspace((unsigned char)*p)) {
    ++p;
  }
  if (*p != ':') {
    return -1;
  }
  ++p;
  while (*p != '\0' && isspace((unsigned char)*p)) {
    ++p;
  }
  if (*p == '-') {
    return -1;
  }
  errno = 0;
  char *end = NULL;
  unsigned long long v = strtoull(p, &end, 10);
  if (errno != 0 || end == p) {
    return -1;
  }
  *out = (uint64_t)v;
  return 0;
}

static int64_t diff_i64(uint64_t a, uint64_t b) {
  if (a >= b) {
    const uint64_t d = a - b;
    return d > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)d;
  }
  const uint64_t d = b - a;
  return d > (uint64_t)INT64_MAX ? INT64_MIN : -(int64_t)d;
}

bk_control *bk_control_connect(const char *path) {
  const char *sock_path = path;
  if (sock_path == NULL) {
    sock_path = getenv("BENCH_CONTROL_SOCK");
  }
  if (sock_path == NULL || sock_path[0] == '\0') {
    return NULL;
  }

  if (strncmp(sock_path, "fd:", 3) == 0) {
    errno = 0;
    char *end = NULL;
    long raw_fd = strtol(sock_path + 3, &end, 10);
    if (errno != 0 || end == sock_path + 3 || *end != '\0' || raw_fd < 0) {
      return NULL;
    }
    int fd = dup((int)raw_fd);
    if (fd < 0) {
      return NULL;
    }
    return control_from_fd(fd);
  }

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    return NULL;
  }

  struct sockaddr_un addr;
  socklen_t addr_len = 0;
  if (fill_sockaddr(sock_path, &addr, &addr_len) != 0) {
    close(fd);
    return NULL;
  }

  if (connect(fd, (const struct sockaddr *)&addr, addr_len) != 0) {
    close(fd);
    return NULL;
  }

  return control_from_fd(fd);
}

void bk_control_close(bk_control *c) {
  if (c == NULL) {
    return;
  }
  if (c->fd >= 0) {
    close(c->fd);
  }
  free(c);
}

int bk_control_hello(bk_control *c, const char *role, const char *transport,
                     int proc_index) {
  if (c == NULL || role == NULL || transport == NULL) {
    return -1;
  }

  char line[1024];
  const int n = snprintf(line, sizeof(line),
                         "{\"type\":\"hello\",\"role\":\"%s\","
                         "\"transport\":\"%s\",\"pid\":%ld,"
                         "\"proc_index\":%d}\n",
                         role, transport, (long)getpid(), proc_index);
  if (n < 0 || (size_t)n >= sizeof(line)) {
    return -1;
  }
  return write_all(c->fd, line, (size_t)n);
}

int bk_control_ready(bk_control *c, int conns) {
  if (c == NULL) {
    return -1;
  }

  char line[128];
  const int n =
      snprintf(line, sizeof(line), "{\"type\":\"ready\",\"conns\":%d}\n", conns);
  if (n < 0 || (size_t)n >= sizeof(line)) {
    return -1;
  }
  return write_all(c->fd, line, (size_t)n);
}

static int parse_schedule_and_ack(bk_control *c, const char *line,
                                  uint64_t recv_ns, bk_schedule *out) {
  if (!json_has_type(line, "\"schedule\"")) {
    return -1;
  }
  if (json_get_u64(line, "\"start_at_ns\"", &out->start_at_ns) != 0 ||
      json_get_u64(line, "\"stop_at_ns\"", &out->stop_at_ns) != 0 ||
      json_get_u64(line, "\"drain_until_ns\"", &out->drain_until_ns) != 0) {
    return -1;
  }

  const int64_t margin_ns = diff_i64(out->start_at_ns, recv_ns);
  char ack[128];
  const int n = snprintf(ack, sizeof(ack),
                         "{\"type\":\"sched_ack\",\"margin_ns\":%" PRId64 "}\n",
                         margin_ns);
  if (n < 0 || (size_t)n >= sizeof(ack)) {
    return -1;
  }
  return write_all(c->fd, ack, (size_t)n);
}

int bk_control_poll_schedule(bk_control *c, bk_schedule *out) {
  if (c == NULL || out == NULL) {
    return -1;
  }

  char line[1024];
  for (;;) {
    const int has = extract_line(c, line, sizeof(line));
    if (has < 0) {
      return -1;
    }
    if (has == 1) {
      break;
    }
    if (c->rlen == sizeof(c->rbuf)) {
      return -1;  // 改行なしで行バッファが溢れた
    }
    const ssize_t n = recv(c->fd, c->rbuf + c->rlen, sizeof(c->rbuf) - c->rlen,
                           MSG_DONTWAIT);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;
      }
      return -1;
    }
    if (n == 0) {
      return -1;  // orchestrator 切断
    }
    c->rlen += (size_t)n;
  }

  const uint64_t recv_ns = bk_now_ns();
  if (parse_schedule_and_ack(c, line, recv_ns, out) != 0) {
    return -1;
  }
  return 1;
}

int bk_control_wait_schedule(bk_control *c, bk_schedule *out) {
  if (c == NULL || out == NULL) {
    return -1;
  }

  for (;;) {
    const int r = bk_control_poll_schedule(c, out);
    if (r != 0) {
      return r == 1 ? 0 : -1;
    }
    struct pollfd pfd;
    pfd.fd = c->fd;
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, -1) < 0 && errno != EINTR) {
      return -1;
    }
  }
}

int bk_control_done(bk_control *c, const char *stats_json) {
  if (c == NULL) {
    return -1;
  }
  if (stats_json == NULL) {
    stats_json = "{}";
  }
  if (write_all(c->fd, "{\"type\":\"done\",\"stats\":", 23) != 0) {
    return -1;
  }
  if (write_all(c->fd, stats_json, strlen(stats_json)) != 0) {
    return -1;
  }
  return write_all(c->fd, "}\n", 2);
}
