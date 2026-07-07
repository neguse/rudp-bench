#include "benchkit.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// raw_udp — v2 ベンチの「ハーネス天井」基準線。信頼性・dedup・輻輳制御を一切
// 持たない生 UDP エコー。このハーネス+benchkit プロトコルで出せる上限を測る
// 参照実装であり、どのトランスポートの capacity も「raw_udp 天井の何%か」で
// 読むためにある(docs/superpowers/specs/2026-07-08-validity-and-tuning.md V1)。

#define RAWUDP_MAX_PAYLOAD_BYTES 65507u  // IPv4 UDP payload の上限
// 256KB SO_RCVBUF/SO_SNDBUF。enet/gns の内部既定および
// adapters/raw_udp の UDP_SOCKET_BUFFER_BYTES と揃える(バッファ増は効かず
// 一部トランスポートを害するという A/B 結果に基づく既定)。
#define RAWUDP_SOCKET_BUFFER_BYTES (256 * 1024)
#define RAWUDP_SERVICE_SLICE_MS 10

typedef enum {
  CLASS_LOSS_TOLERANT = 0,
  CLASS_MUST_DELIVER = 1,
  CLASS_COUNT = 2
} class_index;

typedef enum {
  DIST_ECHO = 0,
  DIST_BROADCAST = 1,
  DIST_COUNT = 2
} dist_index;

typedef struct {
  uint64_t recv[CLASS_COUNT][DIST_COUNT];
  uint64_t recv_measured[CLASS_COUNT][DIST_COUNT];
  uint64_t submit[CLASS_COUNT][DIST_COUNT];
  uint64_t submit_measured[CLASS_COUNT][DIST_COUNT];
  uint64_t send_failed[CLASS_COUNT][DIST_COUNT];
  uint64_t invalid_payload;
} server_stats;

// 学習した接続ピアの表。echo は recvfrom の src へ返すだけなので表を要さないが、
// broadcast は「これまでに1度でも送ってきた全ピア」へ配る必要があるため、
// src アドレスを (addr_key -> 密配列 index) の open-addressing hash で覚える。
typedef struct {
  struct sockaddr_in *addrs;  // 密配列(broadcast の反復対象)
  size_t count;
  size_t cap;
  uint64_t *keys;  // hash slot の key(0 = 空。実ピアは port!=0 で key!=0)
  size_t hcap;     // 2 の冪
} peer_table;

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int signo) {
  (void)signo;
  g_stop = 1;
}

static void print_describe(void) {
  puts("{\"transport\":\"raw_udp\","
       "\"class_mapping\":{\"loss_tolerant\":\"unreliable-udp\","
       "\"must_deliver\":\"unreliable-udp\"},"
       "\"coalescing\":\"none\","
       "\"cc_algo\":\"none\","
       "\"thread_model\":\"single\","
       "\"encryption\":false,"
       "\"max_payload_bytes\":65507,"
       "\"tuning\":[]}");
}

static int parse_u16(const char *s, uint16_t *out) {
  if (s == NULL || *s == '\0') {
    return -1;
  }
  errno = 0;
  char *end = NULL;
  unsigned long v = strtoul(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0' || v > UINT16_MAX) {
    return -1;
  }
  *out = (uint16_t)v;
  return 0;
}

static void usage(const char *argv0) {
  fprintf(stderr, "usage: %s --port PORT\n", argv0);
}

static int parse_args(int argc, char **argv, uint16_t *port) {
  bool have_port = false;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--describe") == 0) {
      print_describe();
      exit(EXIT_SUCCESS);
    }
    if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      if (parse_u16(argv[++i], port) != 0 || *port == 0) {
        return -1;
      }
      have_port = true;
      continue;
    }
    return -1;
  }
  return have_port ? 0 : -1;
}

static class_index class_from_flags(uint8_t flags) {
  return (flags & BK_FLAG_MUST_DELIVER) != 0 ? CLASS_MUST_DELIVER
                                             : CLASS_LOSS_TOLERANT;
}

static dist_index dist_from_flags(uint8_t flags) {
  return (flags & BK_FLAG_BROADCAST) != 0 ? DIST_BROADCAST : DIST_ECHO;
}

// ---- peer_table ------------------------------------------------------------

static uint64_t addr_key(const struct sockaddr_in *a) {
  return ((uint64_t)a->sin_addr.s_addr << 32) | (uint64_t)a->sin_port;
}

static uint64_t hash_u64(uint64_t x) {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ull;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebull;
  x ^= x >> 31;
  return x;
}

static void pt_init(peer_table *pt) {
  memset(pt, 0, sizeof(*pt));
}

static void pt_free(peer_table *pt) {
  free(pt->addrs);
  free(pt->keys);
  memset(pt, 0, sizeof(*pt));
}

static int pt_hash_insert(peer_table *pt, uint64_t key, size_t addr_index) {
  size_t mask = pt->hcap - 1;
  size_t i = (size_t)hash_u64(key) & mask;
  for (;;) {
    if (pt->keys[i] == 0) {
      pt->keys[i] = key;
      // addr_index は addrs 配列内の位置。空 slot に addr_index を紐付ける
      // 必要はなく(broadcast は addrs を線形反復する)、存在判定だけ担う。
      (void)addr_index;
      return 0;
    }
    if (pt->keys[i] == key) {
      return 1;  // 既知
    }
    i = (i + 1) & mask;
  }
}

static int pt_grow_hash(peer_table *pt) {
  size_t new_hcap = pt->hcap == 0 ? 1024 : pt->hcap * 2;
  uint64_t *new_keys = (uint64_t *)calloc(new_hcap, sizeof(*new_keys));
  if (new_keys == NULL) {
    return -1;
  }
  uint64_t *old_keys = pt->keys;
  size_t old_hcap = pt->hcap;
  pt->keys = new_keys;
  pt->hcap = new_hcap;
  for (size_t i = 0; i < old_hcap; ++i) {
    if (old_keys[i] != 0) {
      pt_hash_insert(pt, old_keys[i], 0);
    }
  }
  free(old_keys);
  return 0;
}

// src を観測して未知なら学習する。戻り値 0=ok / -1=allocation 失敗。
static int pt_observe(peer_table *pt, const struct sockaddr_in *src) {
  const uint64_t key = addr_key(src);
  // load factor 0.7 を越えたら rehash
  if (pt->hcap == 0 || (pt->count + 1) * 10 >= pt->hcap * 7) {
    if (pt_grow_hash(pt) != 0) {
      return -1;
    }
  }
  if (pt_hash_insert(pt, key, pt->count) == 1) {
    return 0;  // 既知
  }
  if (pt->count == pt->cap) {
    size_t new_cap = pt->cap == 0 ? 256 : pt->cap * 2;
    struct sockaddr_in *na =
        (struct sockaddr_in *)realloc(pt->addrs, new_cap * sizeof(*na));
    if (na == NULL) {
      return -1;
    }
    pt->addrs = na;
    pt->cap = new_cap;
  }
  pt->addrs[pt->count++] = *src;
  return 0;
}

// ---- socket ----------------------------------------------------------------

static int set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return -1;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void tune_buffers(int fd) {
  const int buf = RAWUDP_SOCKET_BUFFER_BYTES;
  // 効かなくても致命ではない(kernel が丸める)。best-effort。
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
}

static int send_to(int fd, const struct sockaddr_in *dst, const void *data,
                   size_t len) {
  for (;;) {
    ssize_t n = sendto(fd, data, len, 0, (const struct sockaddr *)dst,
                       sizeof(*dst));
    if (n == (ssize_t)len) {
      return 0;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    // 生 UDP は信頼性を持たない。sndbuf 溢れ等での送信失敗はそのまま
    // 送信失敗として数える(=非信頼フロア。上位で救済しない)。
    return -1;
  }
}

static void count_submit(server_stats *stats, class_index cls, dist_index dist,
                         bool measured, bool ok) {
  if (ok) {
    stats->submit[cls][dist]++;
    if (measured) {
      stats->submit_measured[cls][dist]++;
    }
  } else {
    stats->send_failed[cls][dist]++;
  }
}

// 受信 1 発を処理する。payload は無変更のまま echo / broadcast する。
static void handle_datagram(int fd, peer_table *pt, const struct sockaddr_in *src,
                            const uint8_t *data, size_t len,
                            server_stats *stats) {
  bk_header header;
  if (bk_payload_read(data, len, &header) != 0) {
    stats->invalid_payload++;
    return;
  }

  const class_index cls = class_from_flags(header.flags);
  const dist_index dist = dist_from_flags(header.flags);
  const bool measured = (header.flags & BK_FLAG_MEASURE) != 0;
  stats->recv[cls][dist]++;
  if (measured) {
    stats->recv_measured[cls][dist]++;
  }

  if (dist == DIST_ECHO) {
    const bool ok = send_to(fd, src, data, len) == 0;
    count_submit(stats, cls, dist, measured, ok);
    return;
  }

  // broadcast: origin を含む現在の全接続ピアへ同一 payload を配る。
  for (size_t i = 0; i < pt->count; ++i) {
    const bool ok = send_to(fd, &pt->addrs[i], data, len) == 0;
    count_submit(stats, cls, dist, measured, ok);
  }
}

// timeout_ms 待って socket を drain する。戻り値 0=ok / -1=err。
static int service_once(int fd, peer_table *pt, int timeout_ms,
                        server_stats *stats) {
  struct pollfd pfd = {fd, POLLIN, 0};
  const int pr = poll(&pfd, 1, timeout_ms);
  if (pr < 0) {
    if (errno == EINTR) {
      return 0;
    }
    return -1;
  }
  if (pr == 0 || (pfd.revents & POLLIN) == 0) {
    return 0;
  }

  static uint8_t buf[RAWUDP_MAX_PAYLOAD_BYTES];
  for (;;) {
    struct sockaddr_in src;
    socklen_t sl = sizeof(src);
    ssize_t n = recvfrom(fd, buf, sizeof(buf), MSG_TRUNC,
                         (struct sockaddr *)&src, &sl);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;
      }
      return -1;
    }
    if (pt_observe(pt, &src) != 0) {
      return -1;
    }
    // buf に収まらない oversized datagram は drop(invalid 扱い)。
    if ((size_t)n > sizeof(buf)) {
      stats->invalid_payload++;
      continue;
    }
    handle_datagram(fd, pt, &src, buf, (size_t)n, stats);
  }
}

static int format_stats_json(const server_stats *s, char *buf, size_t cap) {
  const int n = snprintf(
      buf, cap,
      "{\"recv\":{\"loss_tolerant\":{\"echo\":%" PRIu64
      ",\"broadcast\":%" PRIu64 "},\"must_deliver\":{\"echo\":%" PRIu64
      ",\"broadcast\":%" PRIu64
      "}},\"submit\":{\"loss_tolerant\":{\"echo\":%" PRIu64
      ",\"broadcast\":%" PRIu64 "},\"must_deliver\":{\"echo\":%" PRIu64
      ",\"broadcast\":%" PRIu64
      "}},\"recv_measured\":{\"loss_tolerant\":{\"echo\":%" PRIu64
      ",\"broadcast\":%" PRIu64 "},\"must_deliver\":{\"echo\":%" PRIu64
      ",\"broadcast\":%" PRIu64
      "}},\"submit_measured\":{\"loss_tolerant\":{\"echo\":%" PRIu64
      ",\"broadcast\":%" PRIu64 "},\"must_deliver\":{\"echo\":%" PRIu64
      ",\"broadcast\":%" PRIu64
      "}},\"send_failed\":{\"loss_tolerant\":{\"echo\":%" PRIu64
      ",\"broadcast\":%" PRIu64 "},\"must_deliver\":{\"echo\":%" PRIu64
      ",\"broadcast\":%" PRIu64 "}},\"invalid_payload\":%" PRIu64 "}",
      s->recv[CLASS_LOSS_TOLERANT][DIST_ECHO],
      s->recv[CLASS_LOSS_TOLERANT][DIST_BROADCAST],
      s->recv[CLASS_MUST_DELIVER][DIST_ECHO],
      s->recv[CLASS_MUST_DELIVER][DIST_BROADCAST],
      s->submit[CLASS_LOSS_TOLERANT][DIST_ECHO],
      s->submit[CLASS_LOSS_TOLERANT][DIST_BROADCAST],
      s->submit[CLASS_MUST_DELIVER][DIST_ECHO],
      s->submit[CLASS_MUST_DELIVER][DIST_BROADCAST],
      s->recv_measured[CLASS_LOSS_TOLERANT][DIST_ECHO],
      s->recv_measured[CLASS_LOSS_TOLERANT][DIST_BROADCAST],
      s->recv_measured[CLASS_MUST_DELIVER][DIST_ECHO],
      s->recv_measured[CLASS_MUST_DELIVER][DIST_BROADCAST],
      s->submit_measured[CLASS_LOSS_TOLERANT][DIST_ECHO],
      s->submit_measured[CLASS_LOSS_TOLERANT][DIST_BROADCAST],
      s->submit_measured[CLASS_MUST_DELIVER][DIST_ECHO],
      s->submit_measured[CLASS_MUST_DELIVER][DIST_BROADCAST],
      s->send_failed[CLASS_LOSS_TOLERANT][DIST_ECHO],
      s->send_failed[CLASS_LOSS_TOLERANT][DIST_BROADCAST],
      s->send_failed[CLASS_MUST_DELIVER][DIST_ECHO],
      s->send_failed[CLASS_MUST_DELIVER][DIST_BROADCAST],
      s->invalid_payload);
  return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

static int write_metrics_out(const char *json) {
  const char *path = getenv("BENCH_METRICS_OUT");
  if (path == NULL || *path == '\0') {
    return 0;
  }
  FILE *f = fopen(path, "w");
  if (f == NULL) {
    return -1;
  }
  const int rc = fputs(json, f) == EOF || fputc('\n', f) == EOF ? -1 : 0;
  if (fclose(f) != 0) {
    return -1;
  }
  return rc;
}

static int open_server_socket(uint16_t port) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    perror("socket");
    return -1;
  }
  int reuse = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
    perror("setsockopt(SO_REUSEADDR)");
    close(fd);
    return -1;
  }
  tune_buffers(fd);
  if (set_nonblock(fd) != 0) {
    perror("fcntl(O_NONBLOCK)");
    close(fd);
    return -1;
  }
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    perror("bind");
    close(fd);
    return -1;
  }
  return fd;
}

int main(int argc, char **argv) {
  uint16_t port = 0;
  if (parse_args(argc, argv, &port) != 0) {
    usage(argv[0]);
    return EXIT_FAILURE;
  }

  if (signal(SIGINT, on_signal) == SIG_ERR ||
      signal(SIGTERM, on_signal) == SIG_ERR) {
    perror("signal");
    return EXIT_FAILURE;
  }

  int fd = open_server_socket(port);
  if (fd < 0) {
    return EXIT_FAILURE;
  }

  peer_table pt;
  pt_init(&pt);
  server_stats stats;
  memset(&stats, 0, sizeof(stats));

  bk_control *control = bk_control_connect(NULL);
  bk_schedule schedule = {0, 0, 0};
  if (control != NULL) {
    if (bk_control_hello(control, "server", "raw_udp", 0) != 0 ||
        bk_control_ready(control, 0) != 0) {
      fprintf(stderr, "benchkit control handshake failed\n");
      bk_control_close(control);
      pt_free(&pt);
      close(fd);
      return EXIT_FAILURE;
    }
    // schedule 待ちの間も socket を drain する。ここでブロックすると client の
    // 登録パケットに応答できず(=broadcast 対象が学習されず)、barrier 全体が
    // 停滞する。
    for (;;) {
      const int r = bk_control_poll_schedule(control, &schedule);
      if (r == 1) {
        break;
      }
      if (r < 0 || g_stop) {
        fprintf(stderr, "benchkit schedule wait failed\n");
        bk_control_close(control);
        pt_free(&pt);
        close(fd);
        return EXIT_FAILURE;
      }
      if (service_once(fd, &pt, RAWUDP_SERVICE_SLICE_MS, &stats) != 0) {
        fprintf(stderr, "service failed\n");
        bk_control_close(control);
        pt_free(&pt);
        close(fd);
        return EXIT_FAILURE;
      }
    }
  }

  bool window_final = false;
  while (!g_stop) {
    int timeout_ms = RAWUDP_SERVICE_SLICE_MS;
    if (control != NULL) {
      const uint64_t now = bk_now_ns();
      // 定常判定つき warmup(benchspec v2): 確定窓(window)を受けたら
      // schedule を差し替える(drain 終端の前倒しに効く)。
      if (!window_final) {
        if (now >= schedule.start_at_ns) {
          window_final = true;
        } else {
          const int wr = bk_control_poll_window(control, &schedule);
          if (wr < 0) {
            fprintf(stderr, "benchkit window poll failed\n");
            bk_control_close(control);
            pt_free(&pt);
            close(fd);
            return EXIT_FAILURE;
          }
          if (wr == 1) {
            window_final = true;
          }
        }
      }
      if (now >= schedule.drain_until_ns) {
        break;
      }
      const uint64_t remain_ns = schedule.drain_until_ns - now;
      const uint64_t remain_ms = remain_ns / 1000000ull;
      if (remain_ms < (uint64_t)timeout_ms) {
        timeout_ms = (int)remain_ms;
      }
    }
    if (service_once(fd, &pt, timeout_ms, &stats) != 0) {
      fprintf(stderr, "service failed\n");
      if (control != NULL) {
        bk_control_close(control);
      }
      pt_free(&pt);
      close(fd);
      return EXIT_FAILURE;
    }
  }

  char stats_json[4096];
  if (format_stats_json(&stats, stats_json, sizeof(stats_json)) != 0) {
    fprintf(stderr, "server stats JSON overflow\n");
    if (control != NULL) {
      bk_control_close(control);
    }
    pt_free(&pt);
    close(fd);
    return EXIT_FAILURE;
  }

  if (write_metrics_out(stats_json) != 0) {
    fprintf(stderr, "failed to write BENCH_METRICS_OUT\n");
  }

  if (control != NULL) {
    if (bk_control_done(control, stats_json) != 0) {
      fprintf(stderr, "benchkit done failed\n");
      bk_control_close(control);
      pt_free(&pt);
      close(fd);
      return EXIT_FAILURE;
    }
    bk_control_close(control);
  }

  pt_free(&pt);
  close(fd);
  return EXIT_SUCCESS;
}
