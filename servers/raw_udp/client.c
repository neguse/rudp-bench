#include "benchkit.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

// raw_udp client — enet client と同一の benchkit 統合(hello/ready/schedule/
// steady/window/metrics/plan、per-send timestamp、dev-mode fallback)。enet
// 固有部分だけを「conn ごとに1 UDP ソケット + connect(2) で server 固定」に
// 置き換えた非信頼フロア実装。

#define RAWUDP_MAX_PAYLOAD_BYTES 65507u
#define RAWUDP_SOCKET_BUFFER_BYTES (256 * 1024)  // server と同一(adapters/raw_udp)
#define RAWUDP_MAX_CONNS 65535u
#define DEV_WARMUP_NS 200000000ull
#define DEV_DURATION_NS 2000000000ull
#define DEV_DRAIN_NS 500000000ull
#define SERVICE_MAX_SLEEP_NS 10000000ull

typedef struct {
  const char *host;
  uint16_t port;
  int conns;
  int proc_index;
  uint32_t origin_base;
  double rate_lt;
  double rate_md;
  bool broadcast_lt;
  bool broadcast_md;
  size_t payload_lt;  // class 別 payload(0 = 未指定)
  size_t payload_md;
  uint64_t deadline_ns;
  uint64_t staleness_period_ns;
} client_config;

typedef struct {
  int fd;                 // server に connect(2) 済みの UDP ソケット
  bk_plan *plan;
  uint32_t origin_id;
  uint32_t local_index;   // 自 proc 内 0 起点(重複判定の受信側キー)
} client_conn;

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

static void usage(const char *argv0) {
  fprintf(stderr,
          "usage: %s --host HOST --port PORT --conns N --proc-index N "
          "--origin-base N --rate-lt HZ --rate-md HZ --payload BYTES "
          "[--payload-lt BYTES] [--payload-md BYTES] "
          "--deadline-ns NS --staleness-period-ns NS\n",
          argv0);
}

static int parse_u64(const char *s, uint64_t *out) {
  if (s == NULL || *s == '\0') {
    return -1;
  }
  errno = 0;
  char *end = NULL;
  unsigned long long v = strtoull(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0') {
    return -1;
  }
  *out = (uint64_t)v;
  return 0;
}

static int parse_u32(const char *s, uint32_t *out) {
  uint64_t v = 0;
  if (parse_u64(s, &v) != 0 || v > UINT32_MAX) {
    return -1;
  }
  *out = (uint32_t)v;
  return 0;
}

static int parse_i32(const char *s, int *out) {
  if (s == NULL || *s == '\0') {
    return -1;
  }
  errno = 0;
  char *end = NULL;
  long v = strtol(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0' || v < INT_MIN || v > INT_MAX) {
    return -1;
  }
  *out = (int)v;
  return 0;
}

static int parse_u16(const char *s, uint16_t *out) {
  uint64_t v = 0;
  if (parse_u64(s, &v) != 0 || v > UINT16_MAX) {
    return -1;
  }
  *out = (uint16_t)v;
  return 0;
}

static int parse_size(const char *s, size_t *out) {
  uint64_t v = 0;
  if (parse_u64(s, &v) != 0 || v > (uint64_t)SIZE_MAX) {
    return -1;
  }
  *out = (size_t)v;
  return 0;
}

static int parse_rate(const char *s, double *out) {
  if (s == NULL || *s == '\0') {
    return -1;
  }
  errno = 0;
  char *end = NULL;
  const double v = strtod(s, &end);
  if (errno != 0 || end == s || *end != '\0' || v < 0.0) {
    return -1;
  }
  *out = v;
  return 0;
}

static int parse_args(int argc, char **argv, client_config *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  bool have_host = false;
  bool have_port = false;
  bool have_conns = false;
  bool have_proc_index = false;
  bool have_origin_base = false;
  bool have_rate_lt = false;
  bool have_rate_md = false;
  bool have_payload = false;
  bool have_deadline = false;
  bool have_staleness = false;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--describe") == 0) {
      print_describe();
      exit(EXIT_SUCCESS);
    }
    if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
      cfg->host = argv[++i];
      have_host = cfg->host[0] != '\0';
      continue;
    }
    if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      have_port = parse_u16(argv[++i], &cfg->port) == 0 && cfg->port != 0;
      continue;
    }
    if (strcmp(argv[i], "--conns") == 0 && i + 1 < argc) {
      have_conns = parse_i32(argv[++i], &cfg->conns) == 0 &&
                   cfg->conns > 0 &&
                   (uint32_t)cfg->conns <= RAWUDP_MAX_CONNS;
      continue;
    }
    if (strcmp(argv[i], "--proc-index") == 0 && i + 1 < argc) {
      have_proc_index = parse_i32(argv[++i], &cfg->proc_index) == 0 &&
                        cfg->proc_index >= 0;
      continue;
    }
    if (strcmp(argv[i], "--origin-base") == 0 && i + 1 < argc) {
      have_origin_base = parse_u32(argv[++i], &cfg->origin_base) == 0;
      continue;
    }
    if (strcmp(argv[i], "--rate-lt") == 0 && i + 1 < argc) {
      have_rate_lt = parse_rate(argv[++i], &cfg->rate_lt) == 0;
      continue;
    }
    if (strcmp(argv[i], "--rate-md") == 0 && i + 1 < argc) {
      have_rate_md = parse_rate(argv[++i], &cfg->rate_md) == 0;
      continue;
    }
    if (strcmp(argv[i], "--broadcast-lt") == 0) {
      cfg->broadcast_lt = true;
      continue;
    }
    if (strcmp(argv[i], "--broadcast-md") == 0) {
      cfg->broadcast_md = true;
      continue;
    }
    if (strcmp(argv[i], "--payload") == 0 && i + 1 < argc) {
      size_t v = 0;
      have_payload = parse_size(argv[++i], &v) == 0;
      cfg->payload_lt = v;
      cfg->payload_md = v;
      continue;
    }
    if (strcmp(argv[i], "--payload-lt") == 0 && i + 1 < argc) {
      have_payload = parse_size(argv[++i], &cfg->payload_lt) == 0;
      continue;
    }
    if (strcmp(argv[i], "--payload-md") == 0 && i + 1 < argc) {
      have_payload = parse_size(argv[++i], &cfg->payload_md) == 0;
      continue;
    }
    if (strcmp(argv[i], "--deadline-ns") == 0 && i + 1 < argc) {
      have_deadline = parse_u64(argv[++i], &cfg->deadline_ns) == 0;
      continue;
    }
    if (strcmp(argv[i], "--staleness-period-ns") == 0 && i + 1 < argc) {
      have_staleness =
          parse_u64(argv[++i], &cfg->staleness_period_ns) == 0 &&
          cfg->staleness_period_ns > 0;
      continue;
    }
    return -1;
  }

  if (!have_host || !have_port || !have_conns || !have_proc_index ||
      !have_origin_base || !have_rate_lt || !have_rate_md || !have_payload ||
      !have_deadline || !have_staleness) {
    return -1;
  }
  if (cfg->rate_lt == 0.0 && cfg->rate_md == 0.0) {
    return -1;
  }
  // 有効な stream の payload が範囲内であること
  if (cfg->rate_lt > 0.0 && (cfg->payload_lt < BK_MIN_PAYLOAD ||
                             cfg->payload_lt > RAWUDP_MAX_PAYLOAD_BYTES)) {
    return -1;
  }
  if (cfg->rate_md > 0.0 && (cfg->payload_md < BK_MIN_PAYLOAD ||
                             cfg->payload_md > RAWUDP_MAX_PAYLOAD_BYTES)) {
    return -1;
  }
  if ((uint64_t)cfg->origin_base + (uint64_t)cfg->conns > UINT32_MAX) {
    return -1;
  }
  return 0;
}

static uint64_t add_ns(uint64_t a, uint64_t b) {
  return UINT64_MAX - a < b ? UINT64_MAX : a + b;
}

static int interval_from_rate(double rate_hz, uint64_t *out) {
  if (rate_hz <= 0.0) {
    return -1;
  }
  const double interval = 1000000000.0 / rate_hz;
  if (interval < 1.0 || interval > (double)UINT64_MAX) {
    return -1;
  }
  uint64_t ns = (uint64_t)(interval + 0.5);
  if (ns == 0) {
    ns = 1;
  }
  *out = ns;
  return 0;
}

static int build_streams(const client_config *cfg, bk_stream *streams,
                         int *n_streams) {
  int n = 0;
  uint64_t interval_ns = 0;
  if (cfg->rate_lt > 0.0) {
    if (interval_from_rate(cfg->rate_lt, &interval_ns) != 0) {
      return -1;
    }
    streams[n++] = (bk_stream){
        .must_deliver = false,
        .broadcast = cfg->broadcast_lt,
        .interval_ns = interval_ns,
    };
  }
  if (cfg->rate_md > 0.0) {
    if (interval_from_rate(cfg->rate_md, &interval_ns) != 0) {
      return -1;
    }
    streams[n++] = (bk_stream){
        .must_deliver = true,
        .broadcast = cfg->broadcast_md,
        .interval_ns = interval_ns,
    };
  }
  *n_streams = n;
  return n > 0 ? 0 : -1;
}

static int set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return -1;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// server に connect(2) 済みの UDP ソケットを作る。以後 send()/recv() は server
// 相手だけになり、他アドレスからの datagram は届かない。
static int open_conn_socket(const struct sockaddr_in *server) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return -1;
  }
  const int buf = RAWUDP_SOCKET_BUFFER_BYTES;
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
  if (connect(fd, (const struct sockaddr *)server, sizeof(*server)) != 0 ||
      set_nonblock(fd) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

// 生 UDP は非信頼。must_deliver も loss_tolerant も同じ send() で送る。ロス下では
// must_deliver も落ちるが、それが raw_udp の正しい挙動(=非信頼フロア)。
static int conn_send(int fd, const void *data, size_t len) {
  for (;;) {
    ssize_t n = send(fd, data, len, 0);
    if (n == (ssize_t)len) {
      return 0;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    return -1;  // sndbuf 溢れ等は送信失敗として扱う(救済しない)
  }
}

static void make_header_from_slot(const bk_slot *slot, uint32_t origin_id,
                                  uint64_t send_ts_ns, bk_header *header) {
  header->seq = slot->seq;
  header->sched_ts_ns = slot->sched_ts_ns;
  header->send_ts_ns = send_ts_ns;
  header->flags = slot->flags;
  header->origin_id = origin_id;
}

static int send_slot(client_conn *conn, const bk_slot *slot,
                     uint8_t *payload_buf, size_t payload_size,
                     bk_metrics *metrics) {
  bk_header header;
  // per-send timestamp: 実送信直前に send_ts を書く(enet client と同様)。
  make_header_from_slot(slot, conn->origin_id, bk_now_ns(), &header);
  if (bk_payload_write(payload_buf, payload_size, &header) != 0) {
    bk_metrics_on_slot(metrics, &header, false);
    return -1;
  }
  const bool submitted = conn_send(conn->fd, payload_buf, payload_size) == 0;
  bk_metrics_on_slot(metrics, &header, submitted);
  return submitted ? 0 : -1;
}

static void mark_unsent_until(client_conn *conn, uint64_t cutoff_ns,
                              bk_metrics *metrics) {
  bk_slot slot;
  while (bk_plan_next(conn->plan, cutoff_ns, &slot)) {
    bk_header header;
    make_header_from_slot(&slot, conn->origin_id, 0, &header);
    bk_metrics_on_slot(metrics, &header, false);
  }
}

// 1 conn の受信 socket を最大 budget パケットまで drain する。
// 戻り値 0=ok / -1=err。**上限が必須**: broadcast fanout(受信 conns² スケール)
// では上限なしだとこのループから抜けられず、main ループの制御チャネル poll
// (bk_steady_tick)と送信が飢えて window を取りこぼす(negative margin で
// INVALID)。budget を超えても POLLIN は残るので次イテレーションで続きを引く。
static int drain_conn(client_conn *conn, bk_metrics *metrics, uint8_t *buf,
                      size_t cap, int budget) {
  for (int drained = 0; drained < budget; ++drained) {
    ssize_t n = recv(conn->fd, buf, cap, 0);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;
      }
      return -1;
    }
    bk_header header;
    if (bk_payload_read(buf, (size_t)n, &header) == 0) {
      bk_metrics_on_recv(metrics, conn->local_index, &header, bk_now_ns());
    }
  }
  return 0;
}

static uint64_t next_plan_due(const client_conn *conns, int n_conns) {
  uint64_t next = UINT64_MAX;
  for (int i = 0; i < n_conns; ++i) {
    const uint64_t due = bk_plan_peek_ns(conns[i].plan);
    if (due < next) {
      next = due;
    }
  }
  return next;
}

// 次に起きるべき時刻までの poll timeout(ms)。SERVICE_MAX_SLEEP_NS で刻む。
static int timeout_for_next(uint64_t now_ns, uint64_t next_ns,
                            uint64_t limit_ns) {
  uint64_t wake_ns = next_ns;
  const uint64_t slice_ns = add_ns(now_ns, SERVICE_MAX_SLEEP_NS);
  if (wake_ns > slice_ns) {
    wake_ns = slice_ns;
  }
  if (wake_ns > limit_ns) {
    wake_ns = limit_ns;
  }
  if (wake_ns <= now_ns) {
    return 0;
  }
  const uint64_t delta_ns = wake_ns - now_ns;
  return (int)(delta_ns / 1000000ull);
}

static char *read_file_alloc(const char *path) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    return NULL;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  const long size = ftell(f);
  if (size < 0) {
    fclose(f);
    return NULL;
  }
  if (fseek(f, 0, SEEK_SET) != 0) {
    fclose(f);
    return NULL;
  }
  char *buf = (char *)malloc((size_t)size + 1u);
  if (buf == NULL) {
    fclose(f);
    return NULL;
  }
  const size_t got = fread(buf, 1, (size_t)size, f);
  if (got != (size_t)size || fclose(f) != 0) {
    free(buf);
    return NULL;
  }
  buf[got] = '\0';
  return buf;
}

static const char *metrics_path_or_default(char *buf, size_t cap) {
  const char *path = getenv("BENCH_METRICS_OUT");
  if (path != NULL && *path != '\0') {
    return path;
  }
  const int n = snprintf(buf, cap, "/tmp/rudp-bench-raw_udp-client-%ld.json",
                         (long)getpid());
  if (n <= 0 || (size_t)n >= cap) {
    return NULL;
  }
  return buf;
}

static void free_conns(client_conn *conns, int n) {
  if (conns == NULL) {
    return;
  }
  for (int i = 0; i < n; ++i) {
    if (conns[i].plan != NULL) {
      bk_plan_free(conns[i].plan);
    }
    if (conns[i].fd >= 0) {
      close(conns[i].fd);
    }
  }
  free(conns);
}

static int run_client(const client_config *cfg) {
  const uint32_t max_origin_id = cfg->origin_base + (uint32_t)cfg->conns;
  const bk_metrics_config metrics_cfg = {
      .max_origin_id = max_origin_id == 0 ? 1u : max_origin_id,
      .deadline_ns = cfg->deadline_ns,
      .staleness_period_ns = cfg->staleness_period_ns,
  };
  bk_metrics *metrics = bk_metrics_new(&metrics_cfg);
  if (metrics == NULL) {
    fprintf(stderr, "bk_metrics_new failed\n");
    return -1;
  }

  struct sockaddr_in server;
  memset(&server, 0, sizeof(server));
  server.sin_family = AF_INET;
  server.sin_port = htons(cfg->port);
  if (inet_pton(AF_INET, cfg->host, &server.sin_addr) != 1) {
    fprintf(stderr, "invalid IPv4 host %s\n", cfg->host);
    bk_metrics_free(metrics);
    return -1;
  }

  client_conn *conns = (client_conn *)calloc((size_t)cfg->conns, sizeof(*conns));
  const size_t payload_buf_size =
      cfg->payload_lt > cfg->payload_md ? cfg->payload_lt : cfg->payload_md;
  uint8_t *payload_buf = (uint8_t *)calloc(1, payload_buf_size);
  uint8_t *recv_buf = (uint8_t *)malloc(RAWUDP_MAX_PAYLOAD_BYTES);
  if (conns == NULL || payload_buf == NULL || recv_buf == NULL) {
    fprintf(stderr, "allocation failed\n");
    free(recv_buf);
    free(payload_buf);
    free(conns);
    bk_metrics_free(metrics);
    return -1;
  }
  for (int i = 0; i < cfg->conns; ++i) {
    conns[i].fd = -1;
  }

  bk_control *control = bk_control_connect(NULL);
  if (control != NULL &&
      bk_control_hello(control, "client", "raw_udp", cfg->proc_index) != 0) {
    fprintf(stderr, "benchkit hello failed\n");
    bk_control_close(control);
    free(recv_buf);
    free(payload_buf);
    free_conns(conns, cfg->conns);
    bk_metrics_free(metrics);
    return -1;
  }

  // conn ごとに socket を作り connect(2) し、登録パケット(header のみ、
  // flags=0 の非計測 payload)を ready より前に1発送って server にアドレスを
  // 学習させる。echo で戻ってくるが flags に MEASURE が無いため metrics は
  // 汚さない(bk_metrics_on_recv は非計測受信を raw カウントのみに回す)。
  for (int i = 0; i < cfg->conns; ++i) {
    conns[i].origin_id = cfg->origin_base + (uint32_t)i;
    conns[i].local_index = (uint32_t)i;
    conns[i].fd = open_conn_socket(&server);
    if (conns[i].fd < 0) {
      fprintf(stderr, "open_conn_socket failed\n");
      if (control != NULL) {
        bk_control_close(control);
      }
      free(recv_buf);
      free(payload_buf);
      free_conns(conns, cfg->conns);
      bk_metrics_free(metrics);
      return -1;
    }
    bk_header reg = {0, 0, bk_now_ns(), 0, conns[i].origin_id};
    if (bk_payload_write(recv_buf, BK_MIN_PAYLOAD, &reg) != 0 ||
        conn_send(conns[i].fd, recv_buf, BK_MIN_PAYLOAD) != 0) {
      // 登録の取りこぼしは致命ではない(warmup 初回送信でも学習される)が、
      // 送信自体の失敗はソケット異常なので中断する。
      fprintf(stderr, "registration send failed\n");
      if (control != NULL) {
        bk_control_close(control);
      }
      free(recv_buf);
      free(payload_buf);
      free_conns(conns, cfg->conns);
      bk_metrics_free(metrics);
      return -1;
    }
  }

  bk_schedule schedule;
  if (control != NULL) {
    if (bk_control_ready(control, cfg->conns) != 0 ||
        bk_control_wait_schedule(control, &schedule) != 0) {
      fprintf(stderr, "benchkit schedule failed\n");
      bk_control_close(control);
      free(recv_buf);
      free(payload_buf);
      free_conns(conns, cfg->conns);
      bk_metrics_free(metrics);
      return -1;
    }
  } else {
    const uint64_t now = bk_now_ns();
    schedule.start_at_ns = add_ns(now, DEV_WARMUP_NS);
    schedule.stop_at_ns = add_ns(schedule.start_at_ns, DEV_DURATION_NS);
    schedule.drain_until_ns = add_ns(schedule.stop_at_ns, DEV_DRAIN_NS);
  }

  bk_stream streams[2];
  int n_streams = 0;
  if (build_streams(cfg, streams, &n_streams) != 0) {
    fprintf(stderr, "invalid rates\n");
    if (control != NULL) {
      bk_control_close(control);
    }
    free(recv_buf);
    free(payload_buf);
    free_conns(conns, cfg->conns);
    bk_metrics_free(metrics);
    return -1;
  }

  const uint64_t plan_start_ns = bk_now_ns();
  for (int i = 0; i < cfg->conns; ++i) {
    conns[i].plan =
        bk_plan_new(streams, n_streams, plan_start_ns, schedule.start_at_ns,
                    schedule.stop_at_ns);
    if (conns[i].plan == NULL) {
      fprintf(stderr, "bk_plan_new failed\n");
      if (control != NULL) {
        bk_control_close(control);
      }
      free(recv_buf);
      free(payload_buf);
      free_conns(conns, cfg->conns);
      bk_metrics_free(metrics);
      return -1;
    }
  }

  // poll セット(全 conn の受信可待ち)。
  struct pollfd *pfds =
      (struct pollfd *)calloc((size_t)cfg->conns, sizeof(*pfds));
  if (pfds == NULL) {
    fprintf(stderr, "allocation failed\n");
    if (control != NULL) {
      bk_control_close(control);
    }
    free(recv_buf);
    free(payload_buf);
    free_conns(conns, cfg->conns);
    bk_metrics_free(metrics);
    return -1;
  }
  for (int i = 0; i < cfg->conns; ++i) {
    pfds[i].fd = conns[i].fd;
    pfds[i].events = POLLIN;
  }

  bool marked_unsent = false;
  int run_rc = 0;
  bk_steady steady = {0, false};
  while (bk_now_ns() < schedule.drain_until_ns) {
    uint64_t now = bk_now_ns();

    // 定常判定つき warmup(benchspec v2): rate 報告と確定窓(window)の受信。
    // window を受けたら全 conn の plan に計測窓を差し替える。
    if (control != NULL) {
      uint64_t raw_submitted = 0;
      uint64_t raw_rm = 0;
      uint64_t raw_ru = 0;
      bk_metrics_raw_counts(metrics, NULL, &raw_submitted, &raw_rm, &raw_ru);
      const int sr = bk_steady_tick(&steady, control, raw_submitted,
                                    raw_rm + raw_ru, &schedule, now);
      if (sr < 0) {
        fprintf(stderr, "benchkit steady tick failed\n");
        run_rc = -1;
        break;
      }
      if (sr == 1) {
        for (int i = 0; i < cfg->conns; ++i) {
          bk_plan_set_window(conns[i].plan, schedule.start_at_ns,
                             schedule.stop_at_ns);
        }
      }
    }
    // staleness サンプルは計測窓内のみ。warmup / drain を混ぜると分布が汚染される。
    if (now >= schedule.start_at_ns && now < schedule.stop_at_ns) {
      bk_metrics_tick(metrics, now);
    }

    if (now < schedule.stop_at_ns) {
      for (int i = 0; i < cfg->conns; ++i) {
        bk_slot slot;
        while (bk_plan_next(conns[i].plan, now, &slot)) {
          const size_t payload_size = (slot.flags & BK_FLAG_MUST_DELIVER)
                                          ? cfg->payload_md
                                          : cfg->payload_lt;
          send_slot(&conns[i], &slot, payload_buf, payload_size, metrics);
        }
      }
    } else if (!marked_unsent) {
      const uint64_t cutoff =
          schedule.stop_at_ns == 0 ? 0 : schedule.stop_at_ns - 1u;
      for (int i = 0; i < cfg->conns; ++i) {
        mark_unsent_until(&conns[i], cutoff, metrics);
      }
      marked_unsent = true;
    }

    now = bk_now_ns();
    uint64_t next_ns = schedule.drain_until_ns;
    if (now < schedule.stop_at_ns) {
      const uint64_t due = next_plan_due(conns, cfg->conns);
      if (due < next_ns) {
        next_ns = due;
      }
    }
    const int timeout_ms =
        timeout_for_next(now, next_ns, schedule.drain_until_ns);
    const int pr = poll(pfds, (nfds_t)cfg->conns, timeout_ms);
    if (pr < 0) {
      if (errno == EINTR) {
        continue;
      }
      fprintf(stderr, "poll failed\n");
      run_rc = -1;
      break;
    }
    if (pr > 0) {
      bool drain_err = false;
      for (int i = 0; i < cfg->conns; ++i) {
        if ((pfds[i].revents & POLLIN) == 0) {
          continue;
        }
        // 1 conn あたりの drain 上限。制御チャネルと送信を飢えさせない範囲で
        // 大きめ(受信のバックログは次イテレーションで続きを引く)。
        if (drain_conn(&conns[i], metrics, recv_buf,
                       RAWUDP_MAX_PAYLOAD_BYTES, 256) != 0) {
          drain_err = true;
          break;
        }
      }
      if (drain_err) {
        fprintf(stderr, "recv failed\n");
        run_rc = -1;
        break;
      }
    }
  }

  if (!marked_unsent) {
    const uint64_t cutoff =
        schedule.stop_at_ns == 0 ? 0 : schedule.stop_at_ns - 1u;
    for (int i = 0; i < cfg->conns; ++i) {
      mark_unsent_until(&conns[i], cutoff, metrics);
    }
  }

  char default_metrics_path[128];
  const char *metrics_path =
      metrics_path_or_default(default_metrics_path, sizeof(default_metrics_path));
  int rc = run_rc;
  if (metrics_path == NULL || bk_metrics_dump_json(metrics, metrics_path) != 0) {
    fprintf(stderr, "bk_metrics_dump_json failed\n");
    rc = -1;
  }

  char *stats_json = metrics_path == NULL ? NULL : read_file_alloc(metrics_path);
  if (control != NULL) {
    if (bk_control_done(control, stats_json != NULL ? stats_json : "{}") != 0) {
      fprintf(stderr, "benchkit done failed\n");
      rc = -1;
    }
    bk_control_close(control);
  }

  free(stats_json);
  free(pfds);
  free(recv_buf);
  free(payload_buf);
  free_conns(conns, cfg->conns);
  bk_metrics_free(metrics);
  return rc;
}

int main(int argc, char **argv) {
  client_config cfg;
  if (parse_args(argc, argv, &cfg) != 0) {
    usage(argv[0]);
    return EXIT_FAILURE;
  }
  return run_client(&cfg) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
