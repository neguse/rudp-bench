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
  size_t payload_lt;  // class 別 payload(0 = 未指定)
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
  uint32_t origin_id;
  uint32_t local_index;   // 自 proc 内 0 起点(重複判定の受信側キー)
  uint64_t last_input_seq;
  uint64_t input_last_sent_measured;
  uint64_t last_applied_input_seq;
  uint64_t state_header_seq_recv_measured;
  uint64_t state_applied_input_seq_recv_measured;
} client_conn;

typedef struct {
  uint64_t invalid_payload;
} client_stats;

static void print_describe(void) {
  puts("{\"transport\":\"raw_udp\","
       "\"class_mapping\":{\"loss_tolerant\":\"unreliable-udp\","
       "\"must_deliver\":\"unreliable-udp\"},"
       "\"coalescing\":\"none\","
       "\"cc_algo\":\"none\","
       "\"thread_model\":\"single\","
       "\"encryption\":false,"
       "\"max_payload_bytes\":65507,"
       "\"scenarios\":[\"environment_baseline\","
       "\"authoritative_state\",\"room_relay\"],"
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
       traffic->payload_lt > RAWUDP_MAX_PAYLOAD_BYTES)) {
    return -1;
  }
  if (traffic->rate_md > 0.0 &&
      (traffic->payload_md < min_payload ||
       traffic->payload_md > RAWUDP_MAX_PAYLOAD_BYTES)) {
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
      print_describe();
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
    if (!have_total_conns || cfg->total_conns > RAWUDP_MAX_CONNS ||
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
  header->traffic_id = slot->traffic_id;
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
  if (bk_payload_fill_body(payload_buf, payload_size, &header) != 0) {
    bk_metrics_on_slot(metrics, &header, false);
    return -1;
  }
  const bool submitted = conn_send(conn->fd, payload_buf, payload_size) == 0;
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

// 1 conn の受信 socket を最大 budget パケットまで drain する。
// 戻り値 0=ok / -1=err。**上限が必須**: broadcast fanout(受信 conns² スケール)
// では上限なしだとこのループから抜けられず、main ループの制御チャネル poll
// (bk_steady_tick)と送信が飢えて window を取りこぼす(negative margin で
// INVALID)。budget を超えても POLLIN は残るので次イテレーションで続きを引く。
static int drain_conn(client_conn *conn, const client_config *cfg,
                      bk_metrics *metrics, uint8_t *buf, size_t cap,
                      int budget, client_stats *stats) {
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
    if (bk_payload_read(buf, (size_t)n, &header) != 0) {
      stats->invalid_payload++;
      continue;
    }
    if (cfg->scenario == SCENARIO_AUTHORITATIVE_STATE) {
        const bool must_deliver =
            (header.flags & BK_FLAG_MUST_DELIVER) != 0;
        const bool class_enabled =
            must_deliver ? cfg->state.rate_md > 0.0
                         : cfg->state.rate_lt > 0.0;
        const size_t expected_size =
            must_deliver ? cfg->state.payload_md : cfg->state.payload_lt;
        if (BK_FLAGS_DIRECTION(header.flags) !=
                BK_DIRECTION_SERVER_TO_CLIENT ||
            header.traffic_id != cfg->state.traffic_id ||
            header.origin_id != cfg->total_conns ||
            (header.flags & BK_FLAG_BROADCAST) != 0 || !class_enabled ||
            (size_t)n != expected_size || header.seq == 0 ||
            header.sched_ts_ns == 0 || header.send_ts_ns == 0) {
          stats->invalid_payload++;
          continue;
        }
        uint64_t applied = 0;
        if (bk_authoritative_state_read_applied_input_seq(
                buf, (size_t)n, &applied) != 0 ||
            applied > conn->last_input_seq ||
            bk_authoritative_state_validate_target_pad(
                buf, (size_t)n, conn->origin_id) != 0) {
          stats->invalid_payload++;
          continue;
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
                  !scenario_client_payload_valid(cfg, &header, (size_t)n)) ||
                 bk_payload_validate_body(buf, (size_t)n, &header) != 0) {
        stats->invalid_payload++;
        continue;
      }
      bk_metrics_on_recv(metrics, conn->local_index, &header, bk_now_ns());
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

static int expect_scenario_flows(const client_config *cfg,
                                 const client_conn *conns,
                                 bk_metrics *metrics,
                                 const bk_schedule *schedule) {
  if (cfg->scenario == SCENARIO_LEGACY) {
    return 0;
  }
  if (cfg->scenario == SCENARIO_AUTHORITATIVE_STATE) {
    if (cfg->state.rate_lt == 0.0) {
      return 0;
    }
    for (int i = 0; i < cfg->conns; ++i) {
      if (bk_metrics_expect_latest(
              metrics, conns[i].local_index, cfg->total_conns,
              cfg->state.traffic_id, BK_DIRECTION_SERVER_TO_CLIENT,
              schedule->start_at_ns) != 0) {
        return -1;
      }
    }
    return 0;
  }
  if (cfg->rate_lt == 0.0) {
    return 0;
  }
  for (int i = 0; i < cfg->conns; ++i) {
    const uint32_t first_origin =
        cfg->scenario == SCENARIO_ROOM_RELAY ? 0u : conns[i].origin_id;
    const uint32_t end_origin =
        cfg->scenario == SCENARIO_ROOM_RELAY ? cfg->total_conns
                                             : first_origin + 1u;
    for (uint32_t origin = first_origin; origin < end_origin; ++origin) {
      if (bk_metrics_expect_latest(metrics, conns[i].local_index, origin,
                                   cfg->traffic_id,
                                   BK_DIRECTION_ROOM_RELAY,
                                   schedule->start_at_ns) != 0) {
        return -1;
      }
    }
  }
  return 0;
}

static int run_client(const client_config *cfg) {
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
    bk_header reg = {
        .seq = 0,
        .sched_ts_ns = 0,
        .send_ts_ns = bk_now_ns(),
        .flags = 0,
        .origin_id = conns[i].origin_id,
        .traffic_id = 0,
    };
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

  if (expect_scenario_flows(cfg, conns, metrics, &schedule) != 0) {
    fprintf(stderr, "bk_metrics_expect_latest failed\n");
    if (control != NULL) {
      bk_control_close(control);
    }
    free(recv_buf);
    free(payload_buf);
    free_conns(conns, cfg->conns);
    bk_metrics_free(metrics);
    return -1;
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
        if (drain_conn(&conns[i], cfg, metrics, recv_buf,
                       RAWUDP_MAX_PAYLOAD_BYTES, 256, &stats) != 0) {
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
