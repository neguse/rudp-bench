#include "kcp_common.h"
#include "../ramp.h"

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
#include <sys/uio.h>
#include <unistd.h>

// kcp client — raw_udp client と同一の benchkit 統合。conn ごとに
// connect(2) 済み UDP socket 1 本と ikcp instance(conv = origin_id)を持ち、
// must-deliver だけを KCP ARQ へ載せる(wire format は kcp_common.h)。

#define KCP_MAX_CONNS 65535u
#define DEV_WARMUP_NS 200000000ull
#define DEV_DURATION_NS 2000000000ull
#define DEV_DRAIN_NS 500000000ull
// KCP interval(5ms)より長く眠ると update/再送が遅れるため slice を合わせる
#define SERVICE_MAX_SLEEP_NS 5000000ull

typedef enum {
  SCENARIO_LEGACY = 0,
  SCENARIO_ENVIRONMENT_BASELINE,
  SCENARIO_AUTHORITATIVE_STATE,
  SCENARIO_ROOM_RELAY,
} scenario_kind;

typedef struct {
  uint8_t traffic_id;
  double rate_lt;
  double rate_md;
  size_t payload_lt;
  size_t payload_md;
  uint64_t deadline_ns;
  unsigned present;
} traffic_config;

#define TRAFFIC_HAVE_ID (1u << 0)
#define TRAFFIC_HAVE_RATE_LT (1u << 1)
#define TRAFFIC_HAVE_RATE_MD (1u << 2)
#define TRAFFIC_HAVE_PAYLOAD_LT (1u << 3)
#define TRAFFIC_HAVE_PAYLOAD_MD (1u << 4)
#define TRAFFIC_HAVE_DEADLINE (1u << 5)
#define TRAFFIC_HAVE_ALL ((1u << 6) - 1u)

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
  size_t payload_lt;
  size_t payload_md;
  uint64_t deadline_ns;
  uint64_t staleness_period_ns;
  scenario_kind scenario;
  uint32_t total_conns;
  uint8_t traffic_id;
  bk_direction direction;
  traffic_config input;
  traffic_config state;
  traffic_config publish;
} client_config;

typedef struct {
  int fd;                 // server に connect(2) 済みの UDP ソケット
  bk_plan *plan;
  ikcpcb *kcp;            // must-deliver channel(conv = origin_id)
  uint32_t origin_id;
  uint32_t local_index;
  uint64_t last_input_seq;
  uint64_t input_last_sent_measured;
  uint64_t last_applied_input_seq;
  uint64_t state_header_seq_recv_measured;
  uint64_t state_applied_input_seq_recv_measured;
} client_conn;

typedef struct {
  uint64_t invalid_payload;
} client_stats;

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

static int parse_scenario(const char *s, scenario_kind *out) {
  if (strcmp(s, "environment_baseline") == 0) {
    *out = SCENARIO_ENVIRONMENT_BASELINE;
  } else if (strcmp(s, "authoritative_state") == 0) {
    *out = SCENARIO_AUTHORITATIVE_STATE;
  } else if (strcmp(s, "room_relay") == 0) {
    *out = SCENARIO_ROOM_RELAY;
  } else {
    return -1;
  }
  return 0;
}

static int parse_traffic_arg(const char *arg, const char *prefix,
                             const char *value, traffic_config *traffic) {
  char name[64];
#define MATCH_TRAFFIC_ARG(suffix)                                        \
  (snprintf(name, sizeof(name), "--%s-%s", prefix, suffix) > 0 &&       \
   strcmp(arg, name) == 0)
  if (MATCH_TRAFFIC_ARG("traffic-id")) {
    uint32_t v = 0;
    if (parse_u32(value, &v) != 0 || v == 0 || v > UINT8_MAX) {
      return -1;
    }
    traffic->traffic_id = (uint8_t)v;
    traffic->present |= TRAFFIC_HAVE_ID;
    return 1;
  }
  if (MATCH_TRAFFIC_ARG("rate-lt")) {
    if (parse_rate(value, &traffic->rate_lt) != 0) {
      return -1;
    }
    traffic->present |= TRAFFIC_HAVE_RATE_LT;
    return 1;
  }
  if (MATCH_TRAFFIC_ARG("rate-md")) {
    if (parse_rate(value, &traffic->rate_md) != 0) {
      return -1;
    }
    traffic->present |= TRAFFIC_HAVE_RATE_MD;
    return 1;
  }
  if (MATCH_TRAFFIC_ARG("payload-lt")) {
    if (parse_size(value, &traffic->payload_lt) != 0) {
      return -1;
    }
    traffic->present |= TRAFFIC_HAVE_PAYLOAD_LT;
    return 1;
  }
  if (MATCH_TRAFFIC_ARG("payload-md")) {
    if (parse_size(value, &traffic->payload_md) != 0) {
      return -1;
    }
    traffic->present |= TRAFFIC_HAVE_PAYLOAD_MD;
    return 1;
  }
  if (MATCH_TRAFFIC_ARG("deadline-ns")) {
    if (parse_u64(value, &traffic->deadline_ns) != 0) {
      return -1;
    }
    traffic->present |= TRAFFIC_HAVE_DEADLINE;
    return 1;
  }
#undef MATCH_TRAFFIC_ARG
  return 0;
}

static int validate_traffic(const traffic_config *traffic,
                            size_t min_payload) {
  if (traffic->present != TRAFFIC_HAVE_ALL ||
      (traffic->rate_lt == 0.0 && traffic->rate_md == 0.0)) {
    return -1;
  }
  if (traffic->rate_lt > 0.0 &&
      (traffic->payload_lt < min_payload ||
       traffic->payload_lt > KCP_MAX_RAW_PAYLOAD_BYTES)) {
    return -1;
  }
  if (traffic->rate_md > 0.0 &&
      (traffic->payload_md < min_payload ||
       traffic->payload_md > KCP_MAX_RAW_PAYLOAD_BYTES)) {
    return -1;
  }
  return 0;
}

static int parse_args(int argc, char **argv, client_config *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->staleness_period_ns = 10000000ull;
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
  bool have_scenario = false;
  bool have_total_conns = false;

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--describe") == 0) {
      puts(kcp_describe_json());
      exit(EXIT_SUCCESS);
    }
    if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
      have_scenario = parse_scenario(argv[++i], &cfg->scenario) == 0;
      continue;
    }
    if (strcmp(argv[i], "--total-conns") == 0 && i + 1 < argc) {
      have_total_conns = parse_u32(argv[++i], &cfg->total_conns) == 0 &&
                         cfg->total_conns > 0;
      continue;
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
                   (uint32_t)cfg->conns <= KCP_MAX_CONNS;
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
    if (i + 1 < argc) {
      int parsed = parse_traffic_arg(argv[i], "input", argv[i + 1],
                                     &cfg->input);
      if (parsed == 0) {
        parsed = parse_traffic_arg(argv[i], "state", argv[i + 1],
                                   &cfg->state);
      }
      if (parsed == 0) {
        parsed = parse_traffic_arg(argv[i], "publish", argv[i + 1],
                                   &cfg->publish);
      }
      if (parsed != 0) {
        if (parsed < 0) {
          return -1;
        }
        ++i;
        continue;
      }
    }
    return -1;
  }

  if (!have_host || !have_port || !have_conns || !have_proc_index ||
      !have_origin_base) {
    return -1;
  }
  if (have_scenario) {
    const traffic_config *outbound = NULL;
    if (!have_total_conns || cfg->total_conns > KCP_MAX_CONNS ||
        (uint64_t)cfg->origin_base + (uint64_t)cfg->conns >
            cfg->total_conns) {
      return -1;
    }
    switch (cfg->scenario) {
      case SCENARIO_ENVIRONMENT_BASELINE:
        outbound = &cfg->input;
        cfg->direction = BK_DIRECTION_ROOM_RELAY;
        break;
      case SCENARIO_AUTHORITATIVE_STATE:
        outbound = &cfg->input;
        cfg->direction = BK_DIRECTION_CLIENT_TO_SERVER;
        if (validate_traffic(&cfg->state,
                             BK_AUTHORITATIVE_STATE_MIN_PAYLOAD) != 0 ||
            cfg->input.traffic_id == cfg->state.traffic_id) {
          return -1;
        }
        break;
      case SCENARIO_ROOM_RELAY:
        outbound = &cfg->publish;
        cfg->direction = BK_DIRECTION_ROOM_RELAY;
        cfg->broadcast_lt = true;
        cfg->broadcast_md = true;
        break;
      case SCENARIO_LEGACY:
        return -1;
    }
    if (validate_traffic(outbound, BK_MIN_PAYLOAD) != 0) {
      return -1;
    }
    cfg->traffic_id = outbound->traffic_id;
    cfg->rate_lt = outbound->rate_lt;
    cfg->rate_md = outbound->rate_md;
    cfg->payload_lt = outbound->payload_lt;
    cfg->payload_md = outbound->payload_md;
    cfg->deadline_ns = outbound->deadline_ns;
    return 0;
  }
  cfg->scenario = SCENARIO_LEGACY;
  cfg->direction = BK_DIRECTION_ROOM_RELAY;
  if (!have_rate_lt || !have_rate_md || !have_payload || !have_deadline ||
      !have_staleness) {
    return -1;
  }
  if (cfg->rate_lt == 0.0 && cfg->rate_md == 0.0) {
    return -1;
  }
  if (cfg->rate_lt > 0.0 && (cfg->payload_lt < BK_MIN_PAYLOAD ||
                             cfg->payload_lt > KCP_MAX_RAW_PAYLOAD_BYTES)) {
    return -1;
  }
  if (cfg->rate_md > 0.0 && (cfg->payload_md < BK_MIN_PAYLOAD ||
                             cfg->payload_md > KCP_MAX_RAW_PAYLOAD_BYTES)) {
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
        .traffic_id = cfg->traffic_id,
        .direction = cfg->direction,
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
        .traffic_id = cfg->traffic_id,
        .direction = cfg->direction,
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

static int open_conn_socket(const struct sockaddr_in *server) {
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return -1;
  }
  const int buf = KCP_SOCKET_BUFFER_BYTES;
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
  if (connect(fd, (const struct sockaddr *)server, sizeof(*server)) != 0 ||
      set_nonblock(fd) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

// loss-tolerant channel の実送信(prefix 0x00 + payload、connected socket)。
static int send_raw(int fd, const void *data, size_t len) {
  uint8_t prefix = KCP_PREFIX_RAW;
  struct iovec iov[2] = {
      {&prefix, KCP_PREFIX_BYTES},
      {(void *)data, len},
  };
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = iov;
  msg.msg_iovlen = 2;
  for (;;) {
    ssize_t n = sendmsg(fd, &msg, 0);
    if (n == (ssize_t)(len + KCP_PREFIX_BYTES)) {
      return 0;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    return -1;
  }
}

// KCP frame の実送信(prefix 0x01 + frame、connected socket)。
static int kcp_output_cb(const char *buf, int len, ikcpcb *kcp, void *user) {
  (void)kcp;
  const client_conn *conn = (const client_conn *)user;
  uint8_t prefix = KCP_PREFIX_KCP;
  struct iovec iov[2] = {
      {&prefix, KCP_PREFIX_BYTES},
      {(void *)buf, (size_t)len},
  };
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = iov;
  msg.msg_iovlen = 2;
  for (;;) {
    const ssize_t n = sendmsg(conn->fd, &msg, 0);
    if (n >= 0) {
      return 0;
    }
    if (errno == EINTR) {
      continue;
    }
    return -1;  // 再送は ARQ に任せる
  }
}

static int setup_conn(client_conn *conn, const client_config *cfg,
                      const struct sockaddr_in *server, int index,
                      uint8_t *registration_buf) {
  conn->origin_id = cfg->origin_base + (uint32_t)index;
  conn->local_index = (uint32_t)index;
  conn->fd = open_conn_socket(server);
  if (conn->fd < 0) {
    return -1;
  }
  conn->kcp = ikcp_create((IUINT32)conn->origin_id, conn);
  if (conn->kcp == NULL) {
    return -1;
  }
  ikcp_setoutput(conn->kcp, kcp_output_cb);
  kcp_apply_tuning(conn->kcp);
  const bk_header registration = {
      .seq = 0,
      .sched_ts_ns = 0,
      .send_ts_ns = bk_now_ns(),
      .flags = 0,
      .origin_id = conn->origin_id,
      .traffic_id = 0,
  };
  return bk_payload_write(registration_buf, BK_MIN_PAYLOAD, &registration) ==
                 0 &&
             send_raw(conn->fd, registration_buf, BK_MIN_PAYLOAD) == 0
         ? 0
         : -1;
}

static int start_conn_plan(client_conn *conn, const bk_stream *streams,
                           int n_streams, uint64_t start_ns,
                           const bk_schedule *schedule) {
  conn->plan = bk_plan_new(streams, n_streams, start_ns,
                           schedule->start_at_ns, schedule->stop_at_ns);
  return conn->plan == NULL ? -1 : 0;
}

static void make_header_from_slot(const bk_slot *slot, uint32_t origin_id,
                                  uint64_t send_ts_ns, bk_header *header) {
  header->seq = slot->seq;
  header->sched_ts_ns = slot->sched_ts_ns;
  header->send_ts_ns = send_ts_ns;
  header->flags = slot->flags;
  header->origin_id = origin_id;
  header->traffic_id = slot->traffic_id;
}

static int send_slot(client_conn *conn, const bk_slot *slot,
                     uint8_t *payload_buf, size_t payload_size,
                     bk_metrics *metrics) {
  bk_header header;
  make_header_from_slot(slot, conn->origin_id, bk_now_ns(), &header);
  if (bk_payload_write(payload_buf, payload_size, &header) != 0) {
    bk_metrics_on_slot(metrics, &header, false);
    return -1;
  }
  if (bk_payload_fill_body(payload_buf, payload_size, &header) != 0) {
    bk_metrics_on_slot(metrics, &header, false);
    return -1;
  }
  bool submitted = false;
  if ((header.flags & BK_FLAG_MUST_DELIVER) != 0) {
    // waitsnd cap は backpressure(bufferbloat 防止)。溢れた slot は未送信。
    submitted = ikcp_waitsnd(conn->kcp) < KCP_MAX_WAITSND &&
                ikcp_send(conn->kcp, (const char *)payload_buf,
                          (int)payload_size) >= 0;
  } else {
    submitted = send_raw(conn->fd, payload_buf, payload_size) == 0;
  }
  bk_metrics_on_slot(metrics, &header, submitted);
  if (submitted && BK_FLAGS_DIRECTION(header.flags) ==
                       BK_DIRECTION_CLIENT_TO_SERVER &&
      (header.flags & BK_FLAG_MUST_DELIVER) == 0 &&
      header.seq > conn->last_input_seq) {
    conn->last_input_seq = header.seq;
  }
  if (submitted && BK_FLAGS_DIRECTION(header.flags) ==
                       BK_DIRECTION_CLIENT_TO_SERVER &&
      (header.flags & (BK_FLAG_MUST_DELIVER | BK_FLAG_MEASURE)) ==
          BK_FLAG_MEASURE &&
      header.seq > conn->input_last_sent_measured) {
    conn->input_last_sent_measured = header.seq;
  }
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

static bool scenario_client_payload_valid(const client_config *cfg,
                                          const bk_header *header,
                                          size_t payload_size) {
  if (cfg->scenario == SCENARIO_LEGACY || header->origin_id >= cfg->total_conns ||
      header->seq == 0 || header->sched_ts_ns == 0 ||
      header->send_ts_ns == 0) {
    return false;
  }
  const traffic_config *traffic = NULL;
  bk_direction direction = BK_DIRECTION_ROOM_RELAY;
  bool broadcast = false;
  switch (cfg->scenario) {
    case SCENARIO_ENVIRONMENT_BASELINE:
      traffic = &cfg->input;
      break;
    case SCENARIO_AUTHORITATIVE_STATE:
      traffic = &cfg->input;
      direction = BK_DIRECTION_CLIENT_TO_SERVER;
      break;
    case SCENARIO_ROOM_RELAY:
      traffic = &cfg->publish;
      broadcast = true;
      break;
    case SCENARIO_LEGACY:
      return false;
  }
  const bool must_deliver =
      (header->flags & BK_FLAG_MUST_DELIVER) != 0;
  const double rate = must_deliver ? traffic->rate_md : traffic->rate_lt;
  const size_t expected_size =
      must_deliver ? traffic->payload_md : traffic->payload_lt;
  return header->traffic_id == traffic->traffic_id &&
         BK_FLAGS_DIRECTION(header->flags) == direction &&
         (((header->flags & BK_FLAG_BROADCAST) != 0) == broadcast) &&
         rate > 0.0 && payload_size == expected_size;
}

// channel demux 済みの benchspec payload を検証して metrics へ入れる。
static void process_payload(client_conn *conn, const client_config *cfg,
                            bk_metrics *metrics, const uint8_t *buf,
                            size_t len, bool from_kcp, client_stats *stats) {
  bk_header header;
  if (bk_payload_read(buf, len, &header) != 0) {
    stats->invalid_payload++;
    return;
  }
  // class と channel の対応(LT=raw / MD=kcp)は wire 契約。外れは invalid。
  if (((header.flags & BK_FLAG_MUST_DELIVER) != 0) != from_kcp) {
    stats->invalid_payload++;
    return;
  }
  if (cfg->scenario == SCENARIO_AUTHORITATIVE_STATE) {
    const bool must_deliver = (header.flags & BK_FLAG_MUST_DELIVER) != 0;
    const bool class_enabled =
        must_deliver ? cfg->state.rate_md > 0.0 : cfg->state.rate_lt > 0.0;
    const size_t expected_size =
        must_deliver ? cfg->state.payload_md : cfg->state.payload_lt;
    if (BK_FLAGS_DIRECTION(header.flags) != BK_DIRECTION_SERVER_TO_CLIENT ||
        header.traffic_id != cfg->state.traffic_id ||
        header.origin_id != cfg->total_conns ||
        (header.flags & BK_FLAG_BROADCAST) != 0 || !class_enabled ||
        len != expected_size || header.seq == 0 ||
        header.sched_ts_ns == 0 || header.send_ts_ns == 0) {
      stats->invalid_payload++;
      return;
    }
    uint64_t applied = 0;
    if (bk_authoritative_state_read_applied_input_seq(buf, len, &applied) !=
            0 ||
        applied > conn->last_input_seq ||
        bk_authoritative_state_validate_target_pad(buf, len,
                                                    conn->origin_id) != 0) {
      stats->invalid_payload++;
      return;
    }
    if (applied > conn->last_applied_input_seq) {
      conn->last_applied_input_seq = applied;
    }
    if (!must_deliver && (header.flags & BK_FLAG_MEASURE) != 0) {
      if (header.seq > conn->state_header_seq_recv_measured) {
        conn->state_header_seq_recv_measured = header.seq;
      }
      if (applied > conn->state_applied_input_seq_recv_measured) {
        conn->state_applied_input_seq_recv_measured = applied;
      }
    }
  } else if ((cfg->scenario != SCENARIO_LEGACY &&
              !scenario_client_payload_valid(cfg, &header, len)) ||
             bk_payload_validate_body(buf, len, &header) != 0) {
    stats->invalid_payload++;
    return;
  }
  bk_metrics_on_recv(metrics, conn->local_index, &header, bk_now_ns());
}

// 1 conn の受信 socket を最大 budget datagram まで drain する(上限必須:
// raw_udp client と同じ飢餓対策)。KCP frame は input 後に完成 message を
// すべて引き切る。
static int drain_conn(client_conn *conn, const client_config *cfg,
                      bk_metrics *metrics, uint8_t *buf, size_t cap,
                      uint8_t *msg_buf, size_t msg_cap, int budget,
                      client_stats *stats) {
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
    if ((size_t)n < KCP_PREFIX_BYTES) {
      stats->invalid_payload++;
      continue;
    }
    const uint8_t prefix = buf[0];
    const uint8_t *body = buf + KCP_PREFIX_BYTES;
    const size_t body_len = (size_t)n - KCP_PREFIX_BYTES;
    if (prefix == KCP_PREFIX_RAW) {
      process_payload(conn, cfg, metrics, body, body_len, false, stats);
    } else if (prefix == KCP_PREFIX_KCP) {
      if (body_len < 4 ||
          ikcp_getconv(body) != (IUINT32)conn->origin_id ||
          ikcp_input(conn->kcp, (const char *)body, (long)body_len) < 0) {
        stats->invalid_payload++;
        continue;
      }
      for (;;) {
        const int m = ikcp_recv(conn->kcp, (char *)msg_buf, (int)msg_cap);
        if (m < 0) {
          break;
        }
        process_payload(conn, cfg, metrics, msg_buf, (size_t)m, true, stats);
      }
    } else {
      stats->invalid_payload++;
    }
  }
  return 0;
}

static int format_client_stats_json(const client_config *cfg,
                                    const client_conn *conns,
                                    const client_stats *stats, char *buf,
                                    size_t cap) {
  uint64_t input_min = 0;
  uint64_t input_max = 0;
  uint64_t header_min = 0;
  uint64_t header_max = 0;
  uint64_t applied_min = 0;
  uint64_t applied_max = 0;
  if (cfg->scenario == SCENARIO_AUTHORITATIVE_STATE) {
    for (int i = 0; i < cfg->conns; ++i) {
      const client_conn *conn = &conns[i];
      if (i == 0 || conn->input_last_sent_measured < input_min) {
        input_min = conn->input_last_sent_measured;
      }
      if (conn->input_last_sent_measured > input_max) {
        input_max = conn->input_last_sent_measured;
      }
      if (i == 0 || conn->state_header_seq_recv_measured < header_min) {
        header_min = conn->state_header_seq_recv_measured;
      }
      if (conn->state_header_seq_recv_measured > header_max) {
        header_max = conn->state_header_seq_recv_measured;
      }
      if (i == 0 ||
          conn->state_applied_input_seq_recv_measured < applied_min) {
        applied_min = conn->state_applied_input_seq_recv_measured;
      }
      if (conn->state_applied_input_seq_recv_measured > applied_max) {
        applied_max = conn->state_applied_input_seq_recv_measured;
      }
    }
  }
  const int n = snprintf(
      buf, cap,
      "{\"invalid_payload\":%" PRIu64
      ",\"authoritative_progress\":{\"role\":\"client\","
      "\"local_conns\":%d,\"roster_conns\":%u,"
      "\"input_last_sent_min\":%" PRIu64
      ",\"input_last_sent_max\":%" PRIu64
      ",\"state_header_seq_recv_min\":%" PRIu64
      ",\"state_header_seq_recv_max\":%" PRIu64
      ",\"state_applied_input_seq_recv_min\":%" PRIu64
      ",\"state_applied_input_seq_recv_max\":%" PRIu64
      ",\"server_state_ticks\":0}}",
      stats->invalid_payload,
      cfg->scenario == SCENARIO_AUTHORITATIVE_STATE ? cfg->conns : 0,
      cfg->scenario == SCENARIO_AUTHORITATIVE_STATE ? cfg->total_conns : 0u,
      input_min, input_max, header_min, header_max, applied_min, applied_max);
  return n > 0 && (size_t)n < cap ? 0 : -1;
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

static void update_kcps(client_conn *conns, int n_conns) {
  const IUINT32 now = kcp_now_ms();
  for (int i = 0; i < n_conns; ++i) {
    if (conns[i].kcp != NULL && ikcp_check(conns[i].kcp, now) <= now) {
      ikcp_update(conns[i].kcp, now);
    }
  }
}

static const char *metrics_path_or_default(char *buf, size_t cap) {
  const char *path = getenv("BENCH_METRICS_OUT");
  if (path != NULL && *path != '\0') {
    return path;
  }
  const int n = snprintf(buf, cap, "/tmp/rudp-bench-kcp-client-%ld.json",
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
    if (conns[i].kcp != NULL) {
      ikcp_release(conns[i].kcp);
    }
    if (conns[i].fd >= 0) {
      close(conns[i].fd);
    }
  }
  free(conns);
}

static int expect_scenario_flows(const client_config *cfg,
                                 const client_conn *conns,
                                 int n_local_conns,
                                 uint32_t roster_conns,
                                 bk_metrics *metrics,
                                 uint64_t first_sched_ts_ns) {
  if (cfg->scenario == SCENARIO_LEGACY) {
    return 0;
  }
  if (cfg->scenario == SCENARIO_AUTHORITATIVE_STATE) {
    if (cfg->state.rate_lt == 0.0) {
      return 0;
    }
    for (int i = 0; i < n_local_conns; ++i) {
      if (bk_metrics_expect_latest(
              metrics, conns[i].local_index, cfg->total_conns,
              cfg->state.traffic_id, BK_DIRECTION_SERVER_TO_CLIENT,
              first_sched_ts_ns) != 0) {
        return -1;
      }
    }
    return 0;
  }
  if (cfg->rate_lt == 0.0) {
    return 0;
  }
  for (int i = 0; i < n_local_conns; ++i) {
    const uint32_t first_origin =
        cfg->scenario == SCENARIO_ROOM_RELAY ? 0u : conns[i].origin_id;
    const uint32_t end_origin =
        cfg->scenario == SCENARIO_ROOM_RELAY ? roster_conns
                                             : first_origin + 1u;
    for (uint32_t origin = first_origin; origin < end_origin; ++origin) {
      if (bk_metrics_expect_latest(metrics, conns[i].local_index, origin,
                                   cfg->traffic_id,
                                   BK_DIRECTION_ROOM_RELAY,
                                   first_sched_ts_ns) != 0) {
        return -1;
      }
    }
  }
  return 0;
}

static int run_client(const client_config *cfg) {
  bk_ramp_config ramp;
  if (bk_ramp_config_load((uint32_t)cfg->conns, &ramp) != 0) {
    fprintf(stderr, "invalid BENCH_RAMP_* configuration\n");
    return -1;
  }
  const uint32_t max_origin_id =
      cfg->scenario == SCENARIO_LEGACY
          ? cfg->origin_base + (uint32_t)cfg->conns
          : cfg->total_conns + 1u;
  const bk_metrics_config metrics_cfg = {
      .max_origin_id = max_origin_id == 0 ? 1u : max_origin_id,
      .deadline_ns = cfg->deadline_ns,
      .staleness_period_ns = cfg->staleness_period_ns,
      .max_local_index = cfg->scenario != SCENARIO_LEGACY
                             ? (uint32_t)cfg->conns
                             : 0,
  };
  bk_metrics *metrics = bk_metrics_new(&metrics_cfg);
  if (metrics == NULL) {
    fprintf(stderr, "bk_metrics_new failed\n");
    return -1;
  }
  if (cfg->scenario == SCENARIO_AUTHORITATIVE_STATE &&
      bk_metrics_set_traffic_deadline(metrics, cfg->state.traffic_id,
                                      BK_DIRECTION_SERVER_TO_CLIENT,
                                      cfg->state.deadline_ns) != 0) {
    fprintf(stderr, "bk_metrics_set_traffic_deadline failed\n");
    bk_metrics_free(metrics);
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
  client_stats stats = {0};
  const size_t payload_buf_size =
      cfg->payload_lt > cfg->payload_md ? cfg->payload_lt : cfg->payload_md;
  uint8_t *payload_buf = (uint8_t *)calloc(1, payload_buf_size);
  uint8_t *recv_buf = (uint8_t *)malloc(KCP_MAX_DATAGRAM_BYTES);
  uint8_t *msg_buf = (uint8_t *)malloc(KCP_MAX_DATAGRAM_BYTES);
  if (conns == NULL || payload_buf == NULL || recv_buf == NULL ||
      msg_buf == NULL) {
    fprintf(stderr, "allocation failed\n");
    free(msg_buf);
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
      bk_control_hello(control, "client", "kcp", cfg->proc_index) != 0) {
    fprintf(stderr, "benchkit hello failed\n");
    bk_control_close(control);
    free(msg_buf);
    free(recv_buf);
    free(payload_buf);
    free_conns(conns, cfg->conns);
    bk_metrics_free(metrics);
    return -1;
  }

  int active_conns = ramp.enabled ? (int)ramp.start_conns : cfg->conns;
  for (int i = 0; i < active_conns; ++i) {
    if (setup_conn(&conns[i], cfg, &server, i, recv_buf) != 0) {
      fprintf(stderr, "connection setup failed\n");
      if (control != NULL) {
        bk_control_close(control);
      }
      free(msg_buf);
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
      free(msg_buf);
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
    free(msg_buf);
    free(recv_buf);
    free(payload_buf);
    free_conns(conns, cfg->conns);
    bk_metrics_free(metrics);
    return -1;
  }

  const uint64_t plan_start_ns = bk_now_ns();
  for (int i = 0; i < active_conns; ++i) {
    if (start_conn_plan(&conns[i], streams, n_streams, plan_start_ns,
                        &schedule) != 0) {
      fprintf(stderr, "bk_plan_new failed\n");
      if (control != NULL) {
        bk_control_close(control);
      }
      free(msg_buf);
      free(recv_buf);
      free(payload_buf);
      free_conns(conns, cfg->conns);
      bk_metrics_free(metrics);
      return -1;
    }
  }

  if (expect_scenario_flows(cfg, conns, active_conns,
                            ramp.enabled ? (uint32_t)active_conns
                                         : cfg->total_conns,
                            metrics, schedule.start_at_ns) != 0) {
    fprintf(stderr, "bk_metrics_expect_latest failed\n");
    if (control != NULL) {
      bk_control_close(control);
    }
    free(msg_buf);
    free(recv_buf);
    free(payload_buf);
    free_conns(conns, cfg->conns);
    bk_metrics_free(metrics);
    return -1;
  }

  struct pollfd *pfds =
      (struct pollfd *)calloc((size_t)cfg->conns, sizeof(*pfds));
  if (pfds == NULL) {
    fprintf(stderr, "allocation failed\n");
    if (control != NULL) {
      bk_control_close(control);
    }
    free(msg_buf);
    free(recv_buf);
    free(payload_buf);
    free_conns(conns, cfg->conns);
    bk_metrics_free(metrics);
    return -1;
  }
  for (int i = 0; i < active_conns; ++i) {
    pfds[i].fd = conns[i].fd;
    pfds[i].events = POLLIN;
  }

  bool marked_unsent = false;
  int run_rc = 0;
  bk_steady steady = {0, false};
  bk_ramp_phase ramp_phase;
  if (ramp.enabled) {
    bk_ramp_phase_begin(&ramp, schedule.start_at_ns, &ramp_phase);
  }
  bool ramp_complete = false;
  while (ramp.enabled ? !ramp_complete
                      : bk_now_ns() < schedule.drain_until_ns) {
    uint64_t now = bk_now_ns();

    if (ramp.enabled && bk_ramp_stop_requested()) {
      ramp_complete = true;
      break;
    }

    if (ramp.enabled && now >= ramp_phase.phase_start_ns) {
      while ((uint32_t)active_conns < ramp_phase.target_conns) {
        if (bk_ramp_stop_requested()) {
          ramp_complete = true;
          break;
        }
        const int index = active_conns;
        if (setup_conn(&conns[index], cfg, &server, index, recv_buf) != 0 ||
            start_conn_plan(&conns[index], streams, n_streams, now,
                            &schedule) != 0) {
          fprintf(stderr, "ramp connection setup failed at %d\n", index);
          run_rc = -1;
          ramp_complete = true;
          break;
        }
        pfds[index].fd = conns[index].fd;
        pfds[index].events = POLLIN;
        active_conns++;
      }
      if (run_rc != 0) {
        break;
      }
      if (ramp_complete) {
        break;
      }
      if (!ramp_phase.reset_done && now >= ramp_phase.phase_end_ns) {
        fprintf(stderr, "ramp phase stalled across reset and phase end\n");
        run_rc = -1;
        break;
      }
      // Prepare accounting as soon as the phase starts, after attempting to
      // add its target sockets. The guard is then entirely available for the
      // peer to reach the same prepared state; guard traffic is rejected by
      // the future-starting cohort.
      if (!ramp_phase.reset_done) {
        bk_metrics_reset(metrics);
        if (bk_metrics_set_cohort(metrics, ramp_phase.reset_at_ns,
                                  ramp_phase.sample_end_ns) != 0 ||
            expect_scenario_flows(cfg, conns,
                                  (int)ramp_phase.target_conns,
                                  ramp_phase.target_conns, metrics,
                                  ramp_phase.reset_at_ns) != 0) {
          fprintf(stderr, "ramp expected-flow registration failed\n");
          run_rc = -1;
          break;
        }
        ramp_phase.sample_conns = ramp_phase.target_conns;
        ramp_phase.reset_done = 1;
      }
      if (now >= ramp_phase.phase_end_ns) {
        const uint64_t cutoff = ramp_phase.sample_end_ns;
        for (uint32_t i = 0; i < ramp_phase.target_conns; ++i) {
          mark_unsent_until(&conns[i], cutoff, metrics);
        }
        if (!ramp_phase.reset_done ||
            bk_ramp_dump_metrics(metrics, ramp_phase.phase,
                                 ramp_phase.sample_conns) != 0) {
          fprintf(stderr, "ramp metrics dump failed\n");
          run_rc = -1;
          break;
        }
        if (bk_ramp_stop_requested()) {
          ramp_complete = true;
          break;
        }
        if (!bk_ramp_phase_advance(&ramp, (uint32_t)cfg->conns,
                                   &ramp_phase)) {
          ramp_complete = true;
          break;
        }
        continue;
      }
    }

    if (control != NULL && !ramp.enabled) {
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
        for (int i = 0; i < active_conns; ++i) {
          bk_plan_set_window(conns[i].plan, schedule.start_at_ns,
                             schedule.stop_at_ns);
        }
      }
    }
    if (now >= schedule.start_at_ns && now < schedule.stop_at_ns) {
      bk_metrics_tick(metrics, now);
    }

    update_kcps(conns, active_conns);

    if (now < schedule.stop_at_ns) {
      for (int i = 0; i < active_conns; ++i) {
        bk_slot slot;
        while (bk_plan_next(conns[i].plan, now, &slot)) {
          const size_t payload_size = (slot.flags & BK_FLAG_MUST_DELIVER)
                                          ? cfg->payload_md
                                          : cfg->payload_lt;
          send_slot(&conns[i], &slot, payload_buf, payload_size, metrics);
        }
      }
    } else if (!ramp.enabled && !marked_unsent) {
      const uint64_t cutoff =
          schedule.stop_at_ns == 0 ? 0 : schedule.stop_at_ns - 1u;
      for (int i = 0; i < active_conns; ++i) {
        bk_slot slot;
        while (bk_plan_next(conns[i].plan, cutoff, &slot)) {
          const size_t payload_size = (slot.flags & BK_FLAG_MUST_DELIVER)
                                          ? cfg->payload_md
                                          : cfg->payload_lt;
          send_slot(&conns[i], &slot, payload_buf, payload_size, metrics);
        }
      }
      for (int i = 0; i < active_conns; ++i) {
        mark_unsent_until(&conns[i], cutoff, metrics);
      }
      marked_unsent = true;
    }

    now = bk_now_ns();
    uint64_t next_ns = schedule.drain_until_ns;
    if (now < schedule.stop_at_ns) {
      const uint64_t due = next_plan_due(conns, active_conns);
      if (due < next_ns) {
        next_ns = due;
      }
    }
    if (ramp.enabled) {
      const uint64_t phase_due = ramp_phase.reset_done
                                     ? ramp_phase.phase_end_ns
                                     : ramp_phase.phase_start_ns;
      if (phase_due < next_ns) {
        next_ns = phase_due;
      }
    }
    const int timeout_ms =
        timeout_for_next(now, next_ns, schedule.drain_until_ns);
    const int pr = poll(pfds, (nfds_t)active_conns, timeout_ms);
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
      for (int i = 0; i < active_conns; ++i) {
        if ((pfds[i].revents & POLLIN) == 0) {
          continue;
        }
        if (drain_conn(&conns[i], cfg, metrics, recv_buf,
                       KCP_MAX_DATAGRAM_BYTES, msg_buf,
                       KCP_MAX_DATAGRAM_BYTES, 256, &stats) != 0) {
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

  if (!ramp.enabled && !marked_unsent) {
    const uint64_t cutoff =
        schedule.stop_at_ns == 0 ? 0 : schedule.stop_at_ns - 1u;
    for (int i = 0; i < cfg->conns; ++i) {
      mark_unsent_until(&conns[i], cutoff, metrics);
    }
  }

  int rc = run_rc;
  if (!ramp.enabled) {
    char default_metrics_path[128];
    const char *metrics_path = metrics_path_or_default(
        default_metrics_path, sizeof(default_metrics_path));
    if (metrics_path == NULL ||
        bk_metrics_dump_json(metrics, metrics_path) != 0) {
      fprintf(stderr, "bk_metrics_dump_json failed\n");
      rc = -1;
    }
  }

  char stats_json[1024];
  if (format_client_stats_json(cfg, conns, &stats, stats_json,
                               sizeof(stats_json)) != 0) {
    fprintf(stderr, "stats JSON formatting failed\n");
    rc = -1;
    stats_json[0] = '{';
    stats_json[1] = '}';
    stats_json[2] = '\0';
  }
  if (control != NULL) {
    if (bk_control_done(control, stats_json) != 0) {
      fprintf(stderr, "benchkit done failed\n");
      rc = -1;
    }
    bk_control_close(control);
  }

  free(pfds);
  free(msg_buf);
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
