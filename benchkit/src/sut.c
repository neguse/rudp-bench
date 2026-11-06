// authoritative-state の server と client(README「劣悪ベンチ」の負荷)。
// wire と metrics の取り決めは benchspec/README.md に従う。
#include "bk_sut.h"

#include "benchkit.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CONNECT_TIMEOUT_NS 10000000000ull
#define MAX_SLEEP_NS 10000000ull

enum { LT = 0, MD = 1 };

typedef struct {
  uint8_t traffic_id;
  double rate[2];
  size_t payload[2];
  uint64_t deadline_ns;
  unsigned present;
} traffic;

typedef struct {
  bool server;
  const char *host;
  uint16_t port;
  int conns;
  int proc_index;
  uint32_t origin_base;
  uint32_t total_conns;
  traffic input;
  traffic state;
  uint64_t staleness_period_ns;
} config;

typedef struct {
  bool connected;
  // client: 自分の origin。server: 登録された origin(未登録は -1)
  int64_t origin;
  bk_plan *plan;
  // client: 送った input(LT)の最大 seq。server: 受けた input(LT)の最大 seq
  uint64_t input_seq;
  uint64_t input_sent_measured;
  uint64_t state_seq_measured;
  uint64_t applied_measured;
} conn_state;

struct bk_sut {
  const bk_transport *tr;
  void *t;
  config cfg;
  bk_metrics *metrics;
  conn_state *conns;
  int n_conns;
  int connected;
  int64_t *roster;  // server: origin → conn(-1 は未登録か切断)
  uint32_t registered;
  bool frozen;
  bool failed;
  uint64_t invalid_payload;
  uint64_t peer_disconnects;
  uint64_t state_ticks;
  int64_t send_queue_max;
  uint8_t *buf;
};

// ---- 引数 ------------------------------------------------------------------

static int parse_u64(const char *s, uint64_t *out) {
  if (s == NULL || *s == '\0') {
    return -1;
  }
  errno = 0;
  char *end = NULL;
  const unsigned long long v = strtoull(s, &end, 10);
  if (errno != 0 || *end != '\0') {
    return -1;
  }
  *out = (uint64_t)v;
  return 0;
}

static int parse_rate(const char *s, double *out) {
  if (s == NULL || *s == '\0') {
    return -1;
  }
  errno = 0;
  char *end = NULL;
  const double v = strtod(s, &end);
  if (errno != 0 || *end != '\0' || !(v >= 0.0)) {
    return -1;
  }
  *out = v;
  return 0;
}

// --<prefix>-<field> を読む。読んだら 1、該当しなければ 0、値が不正なら -1。
static int parse_traffic(const char *arg, const char *value,
                         const char *prefix, traffic *t) {
  const size_t n = strlen(prefix);
  if (strncmp(arg, "--", 2) != 0 || strncmp(arg + 2, prefix, n) != 0 ||
      arg[2 + n] != '-') {
    return 0;
  }
  const char *field = arg + 3 + n;
  uint64_t v = 0;
  if (strcmp(field, "traffic-id") == 0) {
    if (parse_u64(value, &v) != 0 || v == 0 || v > UINT8_MAX) {
      return -1;
    }
    t->traffic_id = (uint8_t)v;
    t->present |= 1u;
  } else if (strcmp(field, "rate-lt") == 0) {
    if (parse_rate(value, &t->rate[LT]) != 0) {
      return -1;
    }
    t->present |= 2u;
  } else if (strcmp(field, "rate-md") == 0) {
    if (parse_rate(value, &t->rate[MD]) != 0) {
      return -1;
    }
    t->present |= 4u;
  } else if (strcmp(field, "payload-lt") == 0) {
    if (parse_u64(value, &v) != 0) {
      return -1;
    }
    t->payload[LT] = (size_t)v;
    t->present |= 8u;
  } else if (strcmp(field, "payload-md") == 0) {
    if (parse_u64(value, &v) != 0) {
      return -1;
    }
    t->payload[MD] = (size_t)v;
    t->present |= 16u;
  } else if (strcmp(field, "deadline-ns") == 0) {
    if (parse_u64(value, &t->deadline_ns) != 0) {
      return -1;
    }
    t->present |= 32u;
  } else {
    return 0;
  }
  return 1;
}

static bool traffic_valid(const traffic *t, size_t min_payload,
                          size_t max_payload) {
  if (t->present != 63u || (t->rate[LT] == 0.0 && t->rate[MD] == 0.0)) {
    return false;
  }
  for (int c = LT; c <= MD; ++c) {
    if (t->rate[c] > 0.0 &&
        (t->payload[c] < min_payload || t->payload[c] > max_payload)) {
      return false;
    }
  }
  return true;
}

static int parse_args(int argc, char **argv, size_t max_payload,
                      config *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->staleness_period_ns = 10000000ull;
  if (argc < 2) {
    return -1;
  }
  cfg->server = strcmp(argv[1], "server") == 0;
  if (!cfg->server && strcmp(argv[1], "client") != 0) {
    return -1;
  }
  bool scenario = false;
  bool have_port = false;
  bool have_conns = false;
  bool have_origin_base = false;
  for (int i = 2; i < argc; ++i) {
    const char *arg = argv[i];
    const char *value = i + 1 < argc ? argv[i + 1] : NULL;
    if (value == NULL) {
      return -1;
    }
    int r = parse_traffic(arg, value, "input", &cfg->input);
    if (r == 0) {
      r = parse_traffic(arg, value, "state", &cfg->state);
    }
    if (r < 0) {
      return -1;
    }
    ++i;
    if (r > 0) {
      continue;
    }
    uint64_t v = 0;
    if (strcmp(arg, "--scenario") == 0) {
      if (strcmp(value, "authoritative_state") != 0) {
        return -1;
      }
      scenario = true;
    } else if (strcmp(arg, "--total-conns") == 0) {
      if (parse_u64(value, &v) != 0 || v == 0 || v > INT32_MAX) {
        return -1;
      }
      cfg->total_conns = (uint32_t)v;
    } else if (strcmp(arg, "--staleness-period-ns") == 0) {
      if (parse_u64(value, &cfg->staleness_period_ns) != 0 ||
          cfg->staleness_period_ns == 0) {
        return -1;
      }
    } else if (strcmp(arg, "--host") == 0) {
      cfg->host = value;
    } else if (strcmp(arg, "--port") == 0) {
      if (parse_u64(value, &v) != 0 || v == 0 || v > UINT16_MAX) {
        return -1;
      }
      cfg->port = (uint16_t)v;
      have_port = true;
    } else if (strcmp(arg, "--conns") == 0) {
      if (parse_u64(value, &v) != 0 || v == 0 || v > INT32_MAX) {
        return -1;
      }
      cfg->conns = (int)v;
      have_conns = true;
    } else if (strcmp(arg, "--proc-index") == 0) {
      if (parse_u64(value, &v) != 0 || v > INT32_MAX) {
        return -1;
      }
      cfg->proc_index = (int)v;
    } else if (strcmp(arg, "--origin-base") == 0) {
      if (parse_u64(value, &v) != 0 || v > UINT32_MAX) {
        return -1;
      }
      cfg->origin_base = (uint32_t)v;
      have_origin_base = true;
    } else {
      return -1;
    }
  }
  if (!scenario || cfg->total_conns == 0 || !have_port ||
      cfg->input.traffic_id == cfg->state.traffic_id ||
      !traffic_valid(&cfg->input, BK_MIN_PAYLOAD, max_payload) ||
      !traffic_valid(&cfg->state, BK_AUTHORITATIVE_STATE_MIN_PAYLOAD,
                     max_payload)) {
    return -1;
  }
  if (!cfg->server &&
      (cfg->host == NULL || !have_conns || !have_origin_base ||
       (uint64_t)cfg->origin_base + (uint64_t)cfg->conns > cfg->total_conns)) {
    return -1;
  }
  return 0;
}

static void print_describe(const bk_transport *tr) {
  printf("{\"transport\":\"%s\",%s,\"payload_pattern\":\"splitmix64-v1\","
         "\"wire_compression\":\"none\",\"max_payload_bytes\":%zu,"
         "\"scenarios\":[\"authoritative_state\"]}\n",
         tr->name, tr->describe, tr->max_payload);
}

// ---- 共通 ------------------------------------------------------------------

static int build_streams(const traffic *t, bk_direction direction,
                         bk_stream *out) {
  int n = 0;
  for (int c = LT; c <= MD; ++c) {
    if (t->rate[c] <= 0.0) {
      continue;
    }
    const double interval = 1e9 / t->rate[c] + 0.5;
    if (interval < 1.0 || interval > 1e18) {
      return -1;
    }
    out[n++] = (bk_stream){
        .must_deliver = c == MD,
        .broadcast = false,
        .traffic_id = t->traffic_id,
        .direction = direction,
        .interval_ns = (uint64_t)interval,
    };
  }
  return n;
}

static bool is_registration(const bk_header *h, size_t len) {
  return len == BK_MIN_PAYLOAD && h->seq == 0 && h->sched_ts_ns == 0 &&
         h->send_ts_ns != 0 && h->flags == 0 && h->traffic_id == 0;
}

static bool header_matches(const bk_header *h, size_t len, const traffic *t,
                           bk_direction direction) {
  const int c = (h->flags & BK_FLAG_MUST_DELIVER) != 0 ? MD : LT;
  return h->seq != 0 && h->sched_ts_ns != 0 && h->send_ts_ns != 0 &&
         h->traffic_id == t->traffic_id &&
         BK_FLAGS_DIRECTION(h->flags) == direction &&
         (h->flags & BK_FLAG_BROADCAST) == 0 && t->rate[c] > 0.0 &&
         len == t->payload[c];
}

static bool valid_conn(const bk_sut *s, int conn) {
  return conn >= 0 && conn < s->n_conns;
}

static uint64_t max_u64(uint64_t a, uint64_t b) { return a > b ? a : b; }

static void sample_send_queue(bk_sut *s) {
  if (s->tr->send_queue == NULL) {
    return;
  }
  for (int i = 0; i < s->n_conns; ++i) {
    if (!s->conns[i].connected) {
      continue;
    }
    const int64_t q = s->tr->send_queue(s->t, i);
    if (q > s->send_queue_max) {
      s->send_queue_max = q;
    }
  }
}

static int service_until(bk_sut *s, uint64_t now, uint64_t next) {
  uint64_t wait = next > now ? next - now : 0;
  if (wait > MAX_SLEEP_NS) {
    wait = MAX_SLEEP_NS;
  }
  if (s->tr->service(s->t, wait) != 0) {
    fprintf(stderr, "%s: service failed\n", s->tr->name);
    return -1;
  }
  return s->failed ? -1 : 0;
}

// schedule を待つ間も送受信を進める(止めると相手側の接続処理が止まる)。
static int wait_schedule(bk_sut *s, bk_control *control,
                         bk_schedule *schedule) {
  for (;;) {
    const int r = bk_control_poll_schedule(control, schedule);
    if (r == 1) {
      return 0;
    }
    const uint64_t now = bk_now_ns();
    if (r < 0 || service_until(s, now, now + MAX_SLEEP_NS) != 0) {
      return -1;
    }
  }
}

static int finish(bk_sut *s, bk_control *control, const char *progress) {
  const char *path = getenv("BENCH_METRICS_OUT");
  if (path == NULL || bk_metrics_dump_json(s->metrics, path) != 0) {
    fprintf(stderr, "%s: metrics dump failed\n", s->tr->name);
    return -1;
  }
  char queue[96] = "";
  if (s->tr->send_queue != NULL) {
    snprintf(queue, sizeof(queue),
             ",\"send_queue_max\":%" PRId64 ",\"send_queue_unit\":\"%s\"",
             s->send_queue_max, s->tr->send_queue_unit);
  }
  char stats[1024];
  const int n = snprintf(stats, sizeof(stats),
                         "{\"invalid_payload\":%" PRIu64
                         ",\"peer_disconnects\":%" PRIu64 "%s,%s}",
                         s->invalid_payload, s->peer_disconnects, queue,
                         progress);
  if (n <= 0 || (size_t)n >= sizeof(stats) ||
      bk_control_done(control, stats) != 0) {
    fprintf(stderr, "%s: done failed\n", s->tr->name);
    return -1;
  }
  return 0;
}

static int format_progress(char *buf, size_t cap, const char *role,
                           uint32_t local_conns, uint32_t roster_conns,
                           const uint64_t lo[3], const uint64_t hi[3],
                           uint64_t ticks) {
  const int n = snprintf(
      buf, cap,
      "\"authoritative_progress\":{\"role\":\"%s\",\"local_conns\":%u,"
      "\"roster_conns\":%u,\"input_last_sent_min\":%" PRIu64
      ",\"input_last_sent_max\":%" PRIu64
      ",\"state_header_seq_recv_min\":%" PRIu64
      ",\"state_header_seq_recv_max\":%" PRIu64
      ",\"state_applied_input_seq_recv_min\":%" PRIu64
      ",\"state_applied_input_seq_recv_max\":%" PRIu64
      ",\"server_state_ticks\":%" PRIu64 "}",
      role, local_conns, roster_conns, lo[0], hi[0], lo[1], hi[1], lo[2],
      hi[2], ticks);
  return n > 0 && (size_t)n < cap ? 0 : -1;
}

// ---- イベント --------------------------------------------------------------

static void send_registration(bk_sut *s, int conn) {
  const bk_header h = {
      .send_ts_ns = bk_now_ns(),
      .origin_id = (uint32_t)s->conns[conn].origin,
  };
  if (bk_payload_write(s->buf, BK_MIN_PAYLOAD, &h) != 0 ||
      s->tr->send(s->t, conn, true, s->buf, BK_MIN_PAYLOAD) != 0) {
    fprintf(stderr, "%s: registration send failed\n", s->tr->name);
    s->failed = true;
  }
}

void bk_sut_on_connect(bk_sut *s, int conn) {
  if (!valid_conn(s, conn) || s->conns[conn].connected) {
    return;
  }
  s->conns[conn].connected = true;
  s->connected++;
  if (!s->cfg.server) {
    send_registration(s, conn);
  }
}

void bk_sut_on_disconnect(bk_sut *s, int conn) {
  if (!valid_conn(s, conn)) {
    return;
  }
  conn_state *c = &s->conns[conn];
  if (!c->connected) {
    // 一度もつながらなかった接続(client の接続失敗)
    if (!s->cfg.server) {
      s->failed = true;
    }
    return;
  }
  c->connected = false;
  s->connected--;
  s->peer_disconnects++;
  fprintf(stderr, "%s: conn %d disconnected (continuing)\n", s->tr->name,
          conn);
  if (s->cfg.server && c->origin >= 0) {
    s->roster[c->origin] = -1;
    if (!s->frozen) {
      s->registered--;
      c->origin = -1;
    }
  }
}

static bool register_origin(bk_sut *s, int conn, uint32_t origin) {
  conn_state *c = &s->conns[conn];
  if (origin >= s->cfg.total_conns) {
    return false;
  }
  if (c->origin >= 0) {
    return c->origin == (int64_t)origin;
  }
  if (s->frozen || s->roster[origin] >= 0) {
    return false;
  }
  c->origin = origin;
  s->roster[origin] = conn;
  s->registered++;
  return true;
}

static bool server_receive(bk_sut *s, int conn, const void *data, size_t len,
                           const bk_header *h) {
  if (is_registration(h, len)) {
    return register_origin(s, conn, h->origin_id);
  }
  if (!header_matches(h, len, &s->cfg.input, BK_DIRECTION_CLIENT_TO_SERVER) ||
      bk_payload_validate_body(data, len, h) != 0 ||
      !register_origin(s, conn, h->origin_id)) {
    return false;
  }
  conn_state *c = &s->conns[conn];
  if ((h->flags & BK_FLAG_MUST_DELIVER) == 0) {
    c->input_seq = max_u64(c->input_seq, h->seq);
  }
  bk_metrics_on_recv(s->metrics, h->origin_id, h, bk_now_ns());
  return true;
}

static bool client_receive(bk_sut *s, int conn, const void *data, size_t len,
                           const bk_header *h) {
  conn_state *c = &s->conns[conn];
  uint64_t applied = 0;
  if (h->origin_id != s->cfg.total_conns ||
      !header_matches(h, len, &s->cfg.state, BK_DIRECTION_SERVER_TO_CLIENT) ||
      bk_authoritative_state_read_applied_input_seq(data, len, &applied) !=
          0 ||
      applied > c->input_seq ||
      bk_authoritative_state_validate_target_pad(data, len,
                                                 (uint32_t)c->origin) != 0) {
    return false;
  }
  if ((h->flags & (BK_FLAG_MUST_DELIVER | BK_FLAG_MEASURE)) ==
      BK_FLAG_MEASURE) {
    c->state_seq_measured = max_u64(c->state_seq_measured, h->seq);
    c->applied_measured = max_u64(c->applied_measured, applied);
  }
  bk_metrics_on_recv(s->metrics, (uint32_t)conn, h, bk_now_ns());
  return true;
}

void bk_sut_on_receive(bk_sut *s, int conn, const void *data, size_t len) {
  bk_header h;
  const bool ok = valid_conn(s, conn) &&
                  bk_payload_read(data, len, &h) == 0 &&
                  (s->cfg.server ? server_receive(s, conn, data, len, &h)
                                 : client_receive(s, conn, data, len, &h));
  if (!ok) {
    s->invalid_payload++;
  }
}

// ---- client ----------------------------------------------------------------

static void client_send_slot(bk_sut *s, int conn, const bk_slot *slot) {
  conn_state *c = &s->conns[conn];
  const bool md = (slot->flags & BK_FLAG_MUST_DELIVER) != 0;
  const size_t len = s->cfg.input.payload[md ? MD : LT];
  const bk_header h = {
      .seq = slot->seq,
      .sched_ts_ns = slot->sched_ts_ns,
      .send_ts_ns = bk_now_ns(),
      .flags = slot->flags,
      .origin_id = (uint32_t)c->origin,
      .traffic_id = slot->traffic_id,
  };
  const bool ok = bk_payload_write(s->buf, len, &h) == 0 &&
                  bk_payload_fill_body(s->buf, len, &h) == 0 &&
                  s->tr->send(s->t, conn, md, s->buf, len) == 0;
  bk_metrics_on_slot(s->metrics, &h, ok);
  if (ok && !md) {
    c->input_seq = max_u64(c->input_seq, h.seq);
    if ((h.flags & BK_FLAG_MEASURE) != 0) {
      c->input_sent_measured = max_u64(c->input_sent_measured, h.seq);
    }
  }
}

static void client_mark_unsent(bk_sut *s, uint64_t cutoff) {
  for (int i = 0; i < s->n_conns; ++i) {
    bk_slot slot;
    while (bk_plan_next(s->conns[i].plan, cutoff, &slot)) {
      const bk_header h = {
          .seq = slot.seq,
          .sched_ts_ns = slot.sched_ts_ns,
          .flags = slot.flags,
          .origin_id = (uint32_t)s->conns[i].origin,
          .traffic_id = slot.traffic_id,
      };
      bk_metrics_on_slot(s->metrics, &h, false);
    }
  }
}

static int client_progress(const bk_sut *s, char *buf, size_t cap) {
  uint64_t lo[3] = {UINT64_MAX, UINT64_MAX, UINT64_MAX};
  uint64_t hi[3] = {0, 0, 0};
  for (int i = 0; i < s->n_conns; ++i) {
    const conn_state *c = &s->conns[i];
    const uint64_t v[3] = {c->input_sent_measured, c->state_seq_measured,
                           c->applied_measured};
    for (int k = 0; k < 3; ++k) {
      lo[k] = v[k] < lo[k] ? v[k] : lo[k];
      hi[k] = max_u64(hi[k], v[k]);
    }
  }
  return format_progress(buf, cap, "client", (uint32_t)s->n_conns,
                         s->cfg.total_conns, lo, hi, 0);
}

static int run_client(bk_sut *s, bk_control *control) {
  const config *cfg = &s->cfg;
  if (bk_control_hello(control, "client", s->tr->name, cfg->proc_index) !=
      0) {
    return -1;
  }
  for (int i = 0; i < s->n_conns; ++i) {
    s->conns[i].origin = cfg->origin_base + (uint32_t)i;
  }
  s->t = s->tr->connect(s, cfg->host, cfg->port, cfg->conns);
  if (s->t == NULL) {
    fprintf(stderr, "%s: connect failed\n", s->tr->name);
    return -1;
  }
  const uint64_t deadline = bk_now_ns() + CONNECT_TIMEOUT_NS;
  while (s->connected < s->n_conns) {
    const uint64_t now = bk_now_ns();
    if (now >= deadline) {
      fprintf(stderr, "%s: connect timed out (%d/%d)\n", s->tr->name,
              s->connected, s->n_conns);
      return -1;
    }
    if (service_until(s, now, now + MAX_SLEEP_NS) != 0) {
      return -1;
    }
  }
  bk_schedule schedule;
  if (bk_control_ready(control, cfg->conns) != 0 ||
      wait_schedule(s, control, &schedule) != 0) {
    return -1;
  }
  bk_stream streams[2];
  const int n_streams =
      build_streams(&cfg->input, BK_DIRECTION_CLIENT_TO_SERVER, streams);
  if (n_streams <= 0) {
    return -1;
  }
  const uint64_t start = bk_now_ns();
  for (int i = 0; i < s->n_conns; ++i) {
    s->conns[i].plan = bk_plan_new(streams, n_streams, start,
                                   schedule.start_at_ns, schedule.stop_at_ns);
    if (s->conns[i].plan == NULL ||
        (cfg->state.rate[LT] > 0.0 &&
         bk_metrics_expect_latest(s->metrics, (uint32_t)i, cfg->total_conns,
                                  cfg->state.traffic_id,
                                  BK_DIRECTION_SERVER_TO_CLIENT,
                                  schedule.start_at_ns) != 0)) {
      return -1;
    }
  }

  bk_steady steady = {0, false};
  for (;;) {
    const uint64_t now = bk_now_ns();
    if (now >= schedule.drain_until_ns) {
      break;
    }
    uint64_t submitted = 0;
    uint64_t recv_measured = 0;
    uint64_t recv_unmeasured = 0;
    bk_metrics_raw_counts(s->metrics, NULL, &submitted, &recv_measured,
                          &recv_unmeasured);
    const int sr = bk_steady_tick(&steady, control, submitted,
                                  recv_measured + recv_unmeasured, &schedule,
                                  now);
    if (sr < 0) {
      return -1;
    }
    if (sr == 1) {
      for (int i = 0; i < s->n_conns; ++i) {
        bk_plan_set_window(s->conns[i].plan, schedule.start_at_ns,
                           schedule.stop_at_ns);
      }
    }
    const bool in_window =
        now >= schedule.start_at_ns && now < schedule.stop_at_ns;
    if (in_window) {
      bk_metrics_tick(s->metrics, now);
      sample_send_queue(s);
    }
    // 計測窓の終わりより前に予定された分は、遅れても送る(遅延は予定時刻から測る)
    const uint64_t cutoff =
        now < schedule.stop_at_ns ? now : schedule.stop_at_ns - 1u;
    uint64_t next = schedule.drain_until_ns;
    bool sent = false;
    for (int i = 0; i < s->n_conns; ++i) {
      if (!s->conns[i].connected) {
        continue;
      }
      bk_slot slot;
      while (bk_plan_next(s->conns[i].plan, cutoff, &slot)) {
        client_send_slot(s, i, &slot);
        sent = true;
      }
      const uint64_t due = bk_plan_peek_ns(s->conns[i].plan);
      if (due < schedule.stop_at_ns && due < next) {
        next = due;
      }
    }
    if (sent && s->tr->flush != NULL) {
      s->tr->flush(s->t);
    }
    if (service_until(s, bk_now_ns(), next) != 0) {
      return -1;
    }
  }
  client_mark_unsent(s, schedule.stop_at_ns - 1u);

  char progress[768];
  if (client_progress(s, progress, sizeof(progress)) != 0) {
    return -1;
  }
  return finish(s, control, progress);
}

// ---- server ----------------------------------------------------------------

static void server_send_slot(bk_sut *s, const bk_slot *slot, bool submit) {
  const bool md = (slot->flags & BK_FLAG_MUST_DELIVER) != 0;
  const size_t len = s->cfg.state.payload[md ? MD : LT];
  if ((slot->flags & (BK_FLAG_MUST_DELIVER | BK_FLAG_MEASURE)) ==
      BK_FLAG_MEASURE) {
    s->state_ticks++;
  }
  for (uint32_t target = 0; target < s->cfg.total_conns; ++target) {
    bk_header h = {
        .seq = slot->seq,
        .sched_ts_ns = slot->sched_ts_ns,
        .flags = slot->flags,
        .origin_id = s->cfg.total_conns,
        .traffic_id = slot->traffic_id,
    };
    const int64_t conn = s->roster[target];
    bool ok = false;
    if (submit && conn >= 0 && s->conns[conn].connected) {
      h.send_ts_ns = bk_now_ns();
      ok = bk_payload_write(s->buf, len, &h) == 0 &&
           bk_authoritative_state_write_applied_input_seq(
               s->buf, len, s->conns[conn].input_seq) == 0 &&
           bk_authoritative_state_fill_target_pad(s->buf, len, target) == 0 &&
           s->tr->send(s->t, (int)conn, md, s->buf, len) == 0;
    }
    bk_metrics_on_slot(s->metrics, &h, ok);
  }
}

static int run_server(bk_sut *s, bk_control *control) {
  const config *cfg = &s->cfg;
  s->t = s->tr->listen(s, cfg->port, s->n_conns);
  if (s->t == NULL) {
    fprintf(stderr, "%s: listen failed on port %u\n", s->tr->name, cfg->port);
    return -1;
  }
  bk_schedule schedule;
  if (bk_control_hello(control, "server", s->tr->name, 0) != 0 ||
      bk_control_ready(control, 0) != 0 ||
      wait_schedule(s, control, &schedule) != 0) {
    return -1;
  }
  bk_stream streams[2];
  const int n_streams =
      build_streams(&cfg->state, BK_DIRECTION_SERVER_TO_CLIENT, streams);
  if (n_streams <= 0) {
    return -1;
  }
  bk_plan *plan = NULL;
  bool window_final = false;
  int rc = 0;
  for (;;) {
    const uint64_t now = bk_now_ns();
    if (now >= schedule.drain_until_ns) {
      break;
    }
    if (!s->frozen) {
      if (s->registered == cfg->total_conns) {
        plan = bk_plan_new(streams, n_streams, now, schedule.start_at_ns,
                           schedule.stop_at_ns);
        if (plan == NULL) {
          rc = -1;
          break;
        }
        for (uint32_t o = 0; o < cfg->total_conns; ++o) {
          if (cfg->input.rate[LT] > 0.0 &&
              bk_metrics_expect_latest(s->metrics, o, o,
                                       cfg->input.traffic_id,
                                       BK_DIRECTION_CLIENT_TO_SERVER,
                                       schedule.start_at_ns) != 0) {
            rc = -1;
          }
        }
        s->frozen = true;
      } else if (now >= schedule.start_at_ns) {
        fprintf(stderr, "%s: only %u/%u clients registered before start\n",
                s->tr->name, s->registered, cfg->total_conns);
        rc = -1;
      }
      if (rc != 0) {
        break;
      }
    }
    if (!window_final) {
      if (now >= schedule.start_at_ns) {
        window_final = true;
      } else {
        const int wr = bk_control_poll_window(control, &schedule);
        if (wr < 0) {
          rc = -1;
          break;
        }
        if (wr == 1) {
          window_final = true;
          if (plan != NULL) {
            bk_plan_set_window(plan, schedule.start_at_ns,
                               schedule.stop_at_ns);
          }
        }
      }
    }
    if (now >= schedule.start_at_ns && now < schedule.stop_at_ns) {
      bk_metrics_tick(s->metrics, now);
      sample_send_queue(s);
    }
    uint64_t next = schedule.drain_until_ns;
    if (plan != NULL) {
      const uint64_t cutoff =
          now < schedule.stop_at_ns ? now : schedule.stop_at_ns - 1u;
      bk_slot slot;
      bool sent = false;
      while (bk_plan_next(plan, cutoff, &slot)) {
        server_send_slot(s, &slot, true);
        sent = true;
      }
      if (sent && s->tr->flush != NULL) {
        s->tr->flush(s->t);
      }
      const uint64_t due = bk_plan_peek_ns(plan);
      if (due < schedule.stop_at_ns) {
        next = due;
      }
    }
    if (service_until(s, bk_now_ns(), next) != 0) {
      rc = -1;
      break;
    }
  }
  if (plan != NULL) {
    bk_slot slot;
    while (bk_plan_next(plan, schedule.stop_at_ns - 1u, &slot)) {
      server_send_slot(s, &slot, false);
    }
    bk_plan_free(plan);
  }
  if (rc != 0) {
    return rc;
  }
  const uint64_t zero[3] = {0, 0, 0};
  char progress[768];
  if (format_progress(progress, sizeof(progress), "server", 0, s->registered,
                      zero, zero, s->state_ticks) != 0) {
    return -1;
  }
  return finish(s, control, progress);
}

// ---- main ------------------------------------------------------------------

int bk_sut_main(int argc, char **argv, const bk_transport *tr) {
  if (argc == 2 && strcmp(argv[1], "--describe") == 0) {
    print_describe(tr);
    return 0;
  }
  bk_sut s;
  memset(&s, 0, sizeof(s));
  s.tr = tr;
  if (parse_args(argc, argv, tr->max_payload, &s.cfg) != 0) {
    fprintf(stderr,
            "usage: %s server --port PORT <scenario args>\n"
            "       %s client --host HOST --port PORT --conns N "
            "--proc-index N --origin-base N <scenario args>\n"
            "       %s --describe\n",
            argv[0], argv[0], argv[0]);
    return 2;
  }
  const config *cfg = &s.cfg;
  s.n_conns = cfg->server ? (int)cfg->total_conns : cfg->conns;
  const bk_metrics_config mcfg = {
      .max_origin_id = cfg->total_conns + 1u,
      .deadline_ns = cfg->input.deadline_ns,
      .staleness_period_ns = cfg->staleness_period_ns,
      .max_local_index = (uint32_t)s.n_conns,
  };
  s.metrics = bk_metrics_new(&mcfg);
  s.conns = calloc((size_t)s.n_conns, sizeof(*s.conns));
  s.roster = calloc(cfg->total_conns, sizeof(*s.roster));
  s.buf = malloc(tr->max_payload);
  bk_control *control = bk_control_connect(NULL);
  int rc = -1;
  if (s.metrics == NULL || s.conns == NULL || s.roster == NULL ||
      s.buf == NULL || control == NULL) {
    fprintf(stderr, "%s: init failed (BENCH_CONTROL_SOCK set?)\n", tr->name);
  } else if (bk_metrics_set_traffic_deadline(
                 s.metrics, cfg->input.traffic_id,
                 BK_DIRECTION_CLIENT_TO_SERVER, cfg->input.deadline_ns) ==
                 0 &&
             bk_metrics_set_traffic_deadline(
                 s.metrics, cfg->state.traffic_id,
                 BK_DIRECTION_SERVER_TO_CLIENT, cfg->state.deadline_ns) ==
                 0) {
    for (int i = 0; i < s.n_conns; ++i) {
      s.conns[i].origin = -1;
    }
    for (uint32_t o = 0; o < cfg->total_conns; ++o) {
      s.roster[o] = -1;
    }
    rc = cfg->server ? run_server(&s, control) : run_client(&s, control);
  }
  if (s.t != NULL) {
    tr->close(s.t);
  }
  if (control != NULL) {
    bk_control_close(control);
  }
  for (int i = 0; s.conns != NULL && i < s.n_conns; ++i) {
    bk_plan_free(s.conns[i].plan);
  }
  free(s.buf);
  free(s.roster);
  free(s.conns);
  bk_metrics_free(s.metrics);
  return rc == 0 ? 0 : 1;
}
