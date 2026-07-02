#include "benchkit.h"

#include <enet/enet.h>

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ENET_BENCH_MAX_PAYLOAD_BYTES 65536u
#define ENET_BENCH_MAX_CONNS 4095u
#define ENET_CHANNEL_RELIABLE 0u
#define ENET_CHANNEL_UNRELIABLE 1u
#define DEV_WARMUP_NS 200000000ull
#define DEV_DURATION_NS 2000000000ull
#define DEV_DRAIN_NS 500000000ull
#define CONNECT_TIMEOUT_NS 10000000000ull
#define SERVICE_MAX_SLEEP_NS 10000000ull

typedef struct {
  const char *host;
  uint16_t port;
  int conns;
  int proc_index;
  uint32_t origin_base;
  double rate_lt;
  double rate_md;
  size_t payload_size;
  uint64_t deadline_ns;
  uint64_t staleness_period_ns;
} client_config;

typedef struct {
  ENetPeer *peer;
  bk_plan *plan;
  uint32_t origin_id;
  bool connected;
} client_conn;

static void print_describe(void) {
  puts("{\"transport\":\"enet\","
       "\"class_mapping\":{\"loss_tolerant\":\"unreliable-unsequenced\","
       "\"must_deliver\":\"reliable\"},"
       "\"coalescing\":\"none\","
       "\"cc_algo\":\"none\","
       "\"thread_model\":\"single\","
       "\"encryption\":false,"
       "\"max_payload_bytes\":65536,"
       "\"tuning\":[]}");
}

static void usage(const char *argv0) {
  fprintf(stderr,
          "usage: %s --host HOST --port PORT --conns N --proc-index N "
          "--origin-base N --rate-lt HZ --rate-md HZ --payload BYTES "
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
                   (uint32_t)cfg->conns <= ENET_BENCH_MAX_CONNS;
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
    if (strcmp(argv[i], "--payload") == 0 && i + 1 < argc) {
      have_payload = parse_size(argv[++i], &cfg->payload_size) == 0 &&
                     cfg->payload_size >= BK_MIN_PAYLOAD &&
                     cfg->payload_size <= ENET_BENCH_MAX_PAYLOAD_BYTES;
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
        .broadcast = false,
        .interval_ns = interval_ns,
    };
  }
  if (cfg->rate_md > 0.0) {
    if (interval_from_rate(cfg->rate_md, &interval_ns) != 0) {
      return -1;
    }
    streams[n++] = (bk_stream){
        .must_deliver = true,
        .broadcast = false,
        .interval_ns = interval_ns,
    };
  }
  *n_streams = n;
  return n > 0 ? 0 : -1;
}

static int enet_class_route(uint8_t flags, enet_uint8 *channel,
                            enet_uint32 *packet_flags) {
  if ((flags & BK_FLAG_MUST_DELIVER) != 0) {
    *channel = ENET_CHANNEL_RELIABLE;
    *packet_flags = ENET_PACKET_FLAG_RELIABLE;
  } else {
    *channel = ENET_CHANNEL_UNRELIABLE;
    *packet_flags = ENET_PACKET_FLAG_UNSEQUENCED;
  }
  return 0;
}

static int send_payload(ENetPeer *peer, const void *data, size_t len,
                        uint8_t flags) {
  enet_uint8 channel = 0;
  enet_uint32 packet_flags = 0;
  if (enet_class_route(flags, &channel, &packet_flags) != 0) {
    return -1;
  }
  ENetPacket *packet = enet_packet_create(data, len, packet_flags);
  if (packet == NULL) {
    return -1;
  }
  if (enet_peer_send(peer, channel, packet) != 0) {
    enet_packet_destroy(packet);
    return -1;
  }
  return 0;
}

static int handle_event(const ENetEvent *event, bk_metrics *metrics,
                        int *connected_count) {
  switch (event->type) {
    case ENET_EVENT_TYPE_CONNECT: {
      client_conn *conn = (client_conn *)event->peer->data;
      if (conn != NULL && !conn->connected) {
        conn->connected = true;
        (*connected_count)++;
      }
      break;
    }
    case ENET_EVENT_TYPE_RECEIVE: {
      bk_header header;
      if (bk_payload_read(event->packet->data, event->packet->dataLength,
                          &header) == 0) {
        bk_metrics_on_recv(metrics, &header, bk_now_ns());
      }
      enet_packet_destroy(event->packet);
      break;
    }
    case ENET_EVENT_TYPE_DISCONNECT:
      return -1;
    case ENET_EVENT_TYPE_NONE:
      break;
  }
  return 0;
}

static int drain_events(ENetHost *host, bk_metrics *metrics,
                        int *connected_count) {
  ENetEvent event;
  for (;;) {
    const int rc = enet_host_service(host, &event, 0);
    if (rc < 0) {
      return -1;
    }
    if (rc == 0) {
      return 0;
    }
    if (handle_event(&event, metrics, connected_count) != 0) {
      return -1;
    }
  }
}

static int wait_for_connects(ENetHost *host, int conns, bk_metrics *metrics) {
  int connected_count = 0;
  const uint64_t deadline = add_ns(bk_now_ns(), CONNECT_TIMEOUT_NS);
  while (connected_count < conns) {
    if (bk_now_ns() >= deadline) {
      return -1;
    }
    ENetEvent event;
    const int rc = enet_host_service(host, &event, 100);
    if (rc < 0) {
      return -1;
    }
    if (rc == 0) {
      continue;
    }
    if (handle_event(&event, metrics, &connected_count) != 0) {
      return -1;
    }
  }
  return 0;
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
  make_header_from_slot(slot, conn->origin_id, bk_now_ns(), &header);
  if (bk_payload_write(payload_buf, payload_size, &header) != 0) {
    bk_metrics_on_slot(metrics, &header, false);
    return -1;
  }
  const bool submitted =
      send_payload(conn->peer, payload_buf, payload_size, header.flags) == 0;
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

static enet_uint32 timeout_for_next(uint64_t now_ns, uint64_t next_ns,
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
  return (enet_uint32)(delta_ns / 1000000ull);
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
  const int n = snprintf(buf, cap, "/tmp/rudp-bench-enet-client-%ld.json",
                         (long)getpid());
  if (n <= 0 || (size_t)n >= cap) {
    return NULL;
  }
  return buf;
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

  ENetHost *host = enet_host_create(NULL, (size_t)cfg->conns, 2, 0, 0);
  if (host == NULL) {
    fprintf(stderr, "enet_host_create client failed\n");
    bk_metrics_free(metrics);
    return -1;
  }

  client_conn *conns = (client_conn *)calloc((size_t)cfg->conns, sizeof(*conns));
  uint8_t *payload_buf = (uint8_t *)calloc(1, cfg->payload_size);
  if (conns == NULL || payload_buf == NULL) {
    fprintf(stderr, "allocation failed\n");
    free(payload_buf);
    free(conns);
    enet_host_destroy(host);
    bk_metrics_free(metrics);
    return -1;
  }

  bk_control *control = bk_control_connect(NULL);
  if (control != NULL &&
      bk_control_hello(control, "client", "enet", cfg->proc_index) != 0) {
    fprintf(stderr, "benchkit hello failed\n");
    bk_control_close(control);
    free(payload_buf);
    free(conns);
    enet_host_destroy(host);
    bk_metrics_free(metrics);
    return -1;
  }

  ENetAddress address;
  memset(&address, 0, sizeof(address));
  if (enet_address_set_host(&address, cfg->host) != 0) {
    fprintf(stderr, "enet_address_set_host failed for %s\n", cfg->host);
    if (control != NULL) {
      bk_control_close(control);
    }
    free(payload_buf);
    free(conns);
    enet_host_destroy(host);
    bk_metrics_free(metrics);
    return -1;
  }
  address.port = cfg->port;

  for (int i = 0; i < cfg->conns; ++i) {
    conns[i].origin_id = cfg->origin_base + (uint32_t)i;
    conns[i].peer = enet_host_connect(host, &address, 2, 0);
    if (conns[i].peer == NULL) {
      fprintf(stderr, "enet_host_connect failed\n");
      if (control != NULL) {
        bk_control_close(control);
      }
      free(payload_buf);
      free(conns);
      enet_host_destroy(host);
      bk_metrics_free(metrics);
      return -1;
    }
    conns[i].peer->data = &conns[i];
  }

  if (wait_for_connects(host, cfg->conns, metrics) != 0) {
    fprintf(stderr, "timed out waiting for ENet connects\n");
    if (control != NULL) {
      bk_control_close(control);
    }
    free(payload_buf);
    free(conns);
    enet_host_destroy(host);
    bk_metrics_free(metrics);
    return -1;
  }

  bk_schedule schedule;
  if (control != NULL) {
    if (bk_control_ready(control, cfg->conns) != 0 ||
        bk_control_wait_schedule(control, &schedule) != 0) {
      fprintf(stderr, "benchkit schedule failed\n");
      bk_control_close(control);
      free(payload_buf);
      free(conns);
      enet_host_destroy(host);
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
    free(payload_buf);
    free(conns);
    enet_host_destroy(host);
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
      for (int j = 0; j < i; ++j) {
        bk_plan_free(conns[j].plan);
      }
      free(payload_buf);
      free(conns);
      enet_host_destroy(host);
      bk_metrics_free(metrics);
      return -1;
    }
  }

  bool marked_unsent = false;
  int connected_count = cfg->conns;
  int run_rc = 0;
  while (bk_now_ns() < schedule.drain_until_ns) {
    uint64_t now = bk_now_ns();
    if (drain_events(host, metrics, &connected_count) != 0) {
      fprintf(stderr, "enet event handling failed\n");
      run_rc = -1;
      break;
    }
    // staleness サンプルは計測窓内のみ。warmup(まだ measured update がない)と
    // drain(送信停止後で age が伸びるだけ)を混ぜると分布が汚染される
    if (now >= schedule.start_at_ns && now < schedule.stop_at_ns) {
      bk_metrics_tick(metrics, now);
    }

    if (now < schedule.stop_at_ns) {
      bool submitted_any = false;
      for (int i = 0; i < cfg->conns; ++i) {
        bk_slot slot;
        while (bk_plan_next(conns[i].plan, now, &slot)) {
          if (send_slot(&conns[i], &slot, payload_buf, cfg->payload_size,
                        metrics) == 0) {
            submitted_any = true;
          }
        }
      }
      if (submitted_any) {
        enet_host_flush(host);
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
    ENetEvent event;
    const int rc =
        enet_host_service(host, &event,
                          timeout_for_next(now, next_ns,
                                           schedule.drain_until_ns));
    if (rc < 0) {
      fprintf(stderr, "enet_host_service failed\n");
      run_rc = -1;
      break;
    }
    if (rc > 0 && handle_event(&event, metrics, &connected_count) != 0) {
      fprintf(stderr, "enet peer disconnected\n");
      run_rc = -1;
      break;
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
  for (int i = 0; i < cfg->conns; ++i) {
    bk_plan_free(conns[i].plan);
  }
  free(payload_buf);
  free(conns);
  enet_host_destroy(host);
  bk_metrics_free(metrics);
  return rc;
}

int main(int argc, char **argv) {
  client_config cfg;
  if (parse_args(argc, argv, &cfg) != 0) {
    usage(argv[0]);
    return EXIT_FAILURE;
  }

  if (enet_initialize() != 0) {
    fprintf(stderr, "enet_initialize failed\n");
    return EXIT_FAILURE;
  }
  atexit(enet_deinitialize);

  return run_client(&cfg) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
