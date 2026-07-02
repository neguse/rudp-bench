#include "benchkit.h"

#include "test_common.h"

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

typedef struct {
  int listen_fd;
  int accepted_fd;
  uint64_t start_at_ns;
  uint64_t stop_at_ns;
  uint64_t drain_until_ns;
} server_args;

static int fill_sockaddr(const char *path, struct sockaddr_un *addr,
                         socklen_t *addr_len) {
  memset(addr, 0, sizeof(*addr));
  addr->sun_family = AF_UNIX;
  const size_t path_len = strlen(path);
  if (path[0] == '@') {
    if (path_len <= 1u || path_len > sizeof(addr->sun_path)) {
      return -1;
    }
    addr->sun_path[0] = '\0';
    memcpy(addr->sun_path + 1, path + 1, path_len - 1u);
    *addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len);
    return 0;
  }
  if (path_len >= sizeof(addr->sun_path)) {
    return -1;
  }
  memcpy(addr->sun_path, path, path_len + 1u);
  *addr_len =
      (socklen_t)(offsetof(struct sockaddr_un, sun_path) + path_len + 1u);
  return 0;
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

static int read_line(int fd, char *buf, size_t cap) {
  size_t len = 0;
  while (len + 1u < cap) {
    char ch;
    ssize_t n = read(fd, &ch, 1);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (n == 0) {
      return -1;
    }
    if (ch == '\n') {
      buf[len] = '\0';
      return 0;
    }
    buf[len++] = ch;
  }
  return -1;
}

static int64_t parse_margin(const char *line) {
  const char *p = strstr(line, "\"margin_ns\":");
  CHECK(p != NULL);
  p += strlen("\"margin_ns\":");
  return (int64_t)strtoll(p, NULL, 10);
}

static void *orchestrator_thread(void *arg) {
  server_args *a = (server_args *)arg;
  int fd = a->accepted_fd >= 0 ? a->accepted_fd : accept(a->listen_fd, NULL, NULL);
  CHECK(fd >= 0);

  char line[2048];
  CHECK(read_line(fd, line, sizeof(line)) == 0);
  CHECK(strstr(line, "\"type\":\"hello\"") != NULL);
  CHECK(strstr(line, "\"role\":\"client\"") != NULL);
  CHECK(strstr(line, "\"transport\":\"null\"") != NULL);
  CHECK(strstr(line, "\"proc_index\":2") != NULL);

  CHECK(read_line(fd, line, sizeof(line)) == 0);
  CHECK(strstr(line, "\"type\":\"ready\"") != NULL);
  CHECK(strstr(line, "\"conns\":3") != NULL);

  char sched[256];
  int n = snprintf(sched, sizeof(sched),
                   "{\"type\":\"schedule\",\"start_at_ns\":%" PRIu64
                   ",\"stop_at_ns\":%" PRIu64 ",\"drain_until_ns\":%" PRIu64
                   "}\n",
                   a->start_at_ns, a->stop_at_ns, a->drain_until_ns);
  CHECK(n > 0 && (size_t)n < sizeof(sched));
  CHECK(write_all(fd, sched, (size_t)n) == 0);

  CHECK(read_line(fd, line, sizeof(line)) == 0);
  CHECK(strstr(line, "\"type\":\"sched_ack\"") != NULL);
  CHECK(parse_margin(line) > 0);

  CHECK(read_line(fd, line, sizeof(line)) == 0);
  CHECK(strstr(line, "\"type\":\"done\"") != NULL);
  CHECK(strstr(line, "\"stats\":{\"ok\":true}") != NULL);

  close(fd);
  return NULL;
}

int main(void) {
  char path[108];
  int n = snprintf(path, sizeof(path), "@benchkit-control-%ld",
                   (long)getpid());
  CHECK(n > 0 && (size_t)n < sizeof(path));

  int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  CHECK(listen_fd >= 0);
  int accepted_fd = -1;

  struct sockaddr_un addr;
  socklen_t addr_len = 0;
  CHECK(fill_sockaddr(path, &addr, &addr_len) == 0);
  if (bind(listen_fd, (const struct sockaddr *)&addr, addr_len) != 0) {
    if (errno != EPERM) {
      fprintf(stderr, "bind %s failed: %s\n", path, strerror(errno));
      CHECK(false);
    }
    close(listen_fd);
    listen_fd = -1;
    int sv[2];
    CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    accepted_fd = sv[1];
    n = snprintf(path, sizeof(path), "fd:%d", sv[0]);
    CHECK(n > 0 && (size_t)n < sizeof(path));
  } else {
    CHECK(listen(listen_fd, 1) == 0);
  }

  server_args args = {
      .listen_fd = listen_fd,
      .accepted_fd = accepted_fd,
      .start_at_ns = bk_now_ns() + 100000000ull,
      .stop_at_ns = bk_now_ns() + 200000000ull,
      .drain_until_ns = bk_now_ns() + 300000000ull,
  };

  pthread_t thread;
  CHECK(pthread_create(&thread, NULL, orchestrator_thread, &args) == 0);

  bk_control *c = bk_control_connect(path);
  CHECK(c != NULL);
  bk_schedule schedule;
  // schedule 未着なら非ブロッキングで 0 が返る
  CHECK(bk_control_poll_schedule(c, &schedule) == 0);
  CHECK(bk_control_hello(c, "client", "null", 2) == 0);
  CHECK(bk_control_ready(c, 3) == 0);
  // server 実装が使うポーリング経路(service ループの合間に呼ぶ形)で受信する
  for (;;) {
    const int r = bk_control_poll_schedule(c, &schedule);
    CHECK(r >= 0);
    if (r == 1) {
      break;
    }
    struct timespec ts = {0, 1000000};  // 1ms
    nanosleep(&ts, NULL);
  }
  CHECK(schedule.start_at_ns == args.start_at_ns);
  CHECK(schedule.stop_at_ns == args.stop_at_ns);
  CHECK(schedule.drain_until_ns == args.drain_until_ns);
  CHECK(bk_control_done(c, "{\"ok\":true}") == 0);
  bk_control_close(c);

  CHECK(pthread_join(thread, NULL) == 0);
  if (listen_fd >= 0) {
    close(listen_fd);
  }
  return 0;
}
