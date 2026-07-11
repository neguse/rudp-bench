#include "benchkit.h"
#include "../scenario_cli.h"

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

// ENet promotes oversized UNSEQUENCED packets to reliable fragmentation.
// Keep the advertised common-class limit below the negotiated MTU so LT never
// changes transport semantics behind the benchmark's back.
#define ENET_BENCH_MAX_PAYLOAD_BYTES 1000u
#define ENET_BENCH_MAX_CONNS 4095u
#define ENET_CHANNEL_RELIABLE 0u
#define ENET_CHANNEL_UNRELIABLE 1u
// 1 回の drain_events で処理するイベント数の上限。broadcast fanout
// (受信 conns² スケール)では上限なしだとループから抜けられず、送信と
// 制御チャネル poll(bk_steady_tick)が飢える(raw_udp の drain 上限と同じ)。
#define ENET_DRAIN_EVENT_BUDGET 4096
#define ENET_BENCH_SOCKBUF_BYTES (4 * 1024 * 1024)
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
  bool broadcast_lt;
  bool broadcast_md;
  size_t payload_lt;  // class 別 payload(0 = 未指定)
  size_t payload_md;
  uint64_t deadline_ns;
  uint64_t staleness_period_ns;
  bk_scenario_cli scenario;
  uint8_t traffic_id;
  bk_direction direction;
} client_config;

typedef struct {
  ENetPeer *peer;
  bk_plan *plan;
  uint32_t origin_id;
  uint32_t local_index;  // 自 proc 内 0 起点(重複判定の受信側キー)
  bool connected;
  uint64_t last_input_seq;
  uint64_t last_applied_input_seq;
  uint64_t input_last_sent_measured;
  uint64_t state_header_seq_recv_measured;
  uint64_t state_applied_input_seq_recv_measured;
} client_conn;

typedef struct {
  uint64_t invalid_payload;
} client_stats;

static void print_describe(void) {
  puts("{\"transport\":\"enet\","
       "\"class_mapping\":{"
       "\"loss_tolerant\":{\"primitive\":\"unreliable-unsequenced\","
       "\"delivery\":\"best_effort\",\"ordering\":\"unordered\","
       "\"realization\":\"native\"},"
       "\"must_deliver\":{\"primitive\":\"reliable\","
       "\"delivery\":\"reliable\",\"ordering\":\"ordered\","
       "\"realization\":\"native\"}},"
       "\"coalescing\":\"none\","
       "\"cc_algo\":\"enet-packet-throttle(scale=32,default=32,accel=32,decel=0,interval=5000ms)\","
       "\"thread_model\":\"single\","
       "\"encryption\":false,"
       "\"max_payload_bytes\":1000,"
       "\"scenarios\":[\"environment_baseline\","
       "\"authoritative_state\",\"room_relay\"],"
       "\"tuning\":["
       "{\"knob\":\"enet_peer_throttle_configure\","
       "\"value\":\"acceleration=32,deceleration=0\","
       "\"upstream_ref\":\"https://github.com/lsalzman/enet/blob/master/include/enet/enet.h\"},"
       "{\"knob\":\"enet_peer_timeout\","
       "\"value\":\"minimum=10s,maximum=60s\","
       "\"upstream_ref\":\"https://github.com/lsalzman/enet/blob/master/include/enet/enet.h\"},"
       "{\"knob\":\"socket_buffers\",\"value\":\"4MiB\","
       "\"upstream_ref\":\"https://man7.org/linux/man-pages/man7/socket.7.html\"},"
       "{\"knob\":\"packet_construction\","
       "\"value\":\"direct-write\","
       "\"upstream_ref\":\"https://github.com/lsalzman/enet/blob/master/include/enet/enet.h\"},"
       "{\"knob\":\"event_drain\","
       "\"value\":\"enet_host_check_events\","
       "\"upstream_ref\":\"https://github.com/lsalzman/enet/blob/master/include/enet/enet.h\"}]}");
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

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--describe") == 0) {
      print_describe();
      exit(EXIT_SUCCESS);
    }
    const int scenario_arg = bk_scenario_cli_parse(argc, argv, &i,
                                                   &cfg->scenario);
    if (scenario_arg < 0) {
      return -1;
    }
    if (scenario_arg > 0) {
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
      !have_origin_base) {
    return -1;
  }
  if (cfg->scenario.present) {
    if (bk_scenario_cli_validate(&cfg->scenario, ENET_BENCH_MAX_CONNS,
                                 ENET_BENCH_MAX_PAYLOAD_BYTES) != 0 ||
        (uint64_t)cfg->origin_base + (uint64_t)cfg->conns >
            cfg->scenario.total_conns) {
      return -1;
    }
    int broadcast = 0;
    const bk_scenario_traffic *traffic = bk_scenario_client_traffic(
        &cfg->scenario, &cfg->direction, &broadcast);
    if (traffic == NULL) {
      return -1;
    }
    cfg->traffic_id = traffic->traffic_id;
    cfg->rate_lt = traffic->rate_lt;
    cfg->rate_md = traffic->rate_md;
    cfg->payload_lt = traffic->payload_lt;
    cfg->payload_md = traffic->payload_md;
    cfg->deadline_ns = traffic->deadline_ns;
    cfg->broadcast_lt = broadcast != 0;
    cfg->broadcast_md = broadcast != 0;
    return 0;
  }
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
                             cfg->payload_lt > ENET_BENCH_MAX_PAYLOAD_BYTES)) {
    return -1;
  }
  if (cfg->rate_md > 0.0 && (cfg->payload_md < BK_MIN_PAYLOAD ||
                             cfg->payload_md > ENET_BENCH_MAX_PAYLOAD_BYTES)) {
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

static enet_uint8 channel_from_flags(uint8_t flags) {
  return (flags & BK_FLAG_MUST_DELIVER) != 0 ? ENET_CHANNEL_RELIABLE
                                             : ENET_CHANNEL_UNRELIABLE;
}

static bool route_matches_header(const ENetEvent *event,
                                 const bk_header *header) {
  const enet_uint32 class_mask =
      ENET_PACKET_FLAG_RELIABLE | ENET_PACKET_FLAG_UNSEQUENCED;
  const enet_uint32 expected =
      (header->flags & BK_FLAG_MUST_DELIVER) != 0
          ? ENET_PACKET_FLAG_RELIABLE
          : ENET_PACKET_FLAG_UNSEQUENCED;
  return event->channelID == channel_from_flags(header->flags) &&
         (event->packet->flags & class_mask) == expected;
}

static void enet_class_route(uint8_t flags, enet_uint8 *channel,
                             enet_uint32 *packet_flags) {
  *channel = channel_from_flags(flags);
  if ((flags & BK_FLAG_MUST_DELIVER) != 0) {
    *packet_flags = ENET_PACKET_FLAG_RELIABLE;
  } else {
    *packet_flags = ENET_PACKET_FLAG_UNSEQUENCED;
  }
}

// peer 単位チューニング(upstream 公式 API。--describe の tuning に開示):
// - packet throttle: 減速を無効化。既定は burst loss で throttle が 32→0 に
//   滑落し unreliable の大半を送信キューで自己破棄する(docs/ledger.md #11)
// - timeout: 高負荷で service が間引かれた際の reliable timeout 切断
//   (ledger #8 の client crash)を遅らせる
static void tune_peer(ENetPeer *peer) {
  enet_peer_throttle_configure(peer, ENET_PEER_PACKET_THROTTLE_INTERVAL,
                               ENET_PEER_PACKET_THROTTLE_SCALE, 0);
  enet_peer_timeout(peer, ENET_PEER_TIMEOUT_LIMIT, 10000, 60000);
}

static int handle_event(const ENetEvent *event, const client_config *cfg,
                        bk_metrics *metrics, int *connected_count,
                        client_stats *stats) {
  switch (event->type) {
    case ENET_EVENT_TYPE_CONNECT: {
      client_conn *conn = (client_conn *)event->peer->data;
      if (conn != NULL && !conn->connected) {
        conn->connected = true;
        (*connected_count)++;
        tune_peer(event->peer);
      }
      break;
    }
    case ENET_EVENT_TYPE_RECEIVE: {
      bk_header header;
      client_conn *conn = (client_conn *)event->peer->data;
      bool accept = conn != NULL &&
                    bk_payload_read(event->packet->data,
                                    event->packet->dataLength, &header) == 0;
      if (accept) {
        accept = route_matches_header(event, &header);
      }
      if (accept &&
          cfg->scenario.kind == BK_SCENARIO_AUTHORITATIVE_STATE) {
        const bool must_deliver =
            (header.flags & BK_FLAG_MUST_DELIVER) != 0;
        uint64_t applied = 0;
        accept = bk_scenario_state_payload_valid(
                     &cfg->scenario, &header, event->packet->dataLength) &&
                 bk_authoritative_state_read_applied_input_seq(
                     event->packet->data, event->packet->dataLength,
                     &applied) == 0 &&
                 applied <= conn->last_input_seq &&
                 bk_authoritative_state_validate_target_pad(
                     event->packet->data, event->packet->dataLength,
                     conn->origin_id) == 0;
        if (accept && applied > conn->last_applied_input_seq) {
          conn->last_applied_input_seq = applied;
        }
        if (accept && !must_deliver &&
            (header.flags & BK_FLAG_MEASURE) != 0) {
          if (header.seq > conn->state_header_seq_recv_measured) {
            conn->state_header_seq_recv_measured = header.seq;
          }
          if (applied > conn->state_applied_input_seq_recv_measured) {
            conn->state_applied_input_seq_recv_measured = applied;
          }
        }
      } else if (accept) {
        if (cfg->scenario.present) {
          accept = bk_scenario_client_payload_valid(
              &cfg->scenario, &header, event->packet->dataLength);
        }
        if (accept) {
          accept = bk_payload_validate_body(event->packet->data,
                                            event->packet->dataLength,
                                            &header) == 0;
        }
      }
      if (accept) {
        bk_metrics_on_recv(metrics, conn->local_index, &header, bk_now_ns());
      } else {
        stats->invalid_payload++;
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

// dispatch 済みイベントは enet_host_check_events で I/O なしに引く。
// enet_host_service は 1 呼び出しで最大 1 event しか返さず、毎回全 peer 走査の
// 送信パス×2 + 受信を回すため(protocol.c:1830,1846,1862)、event ごとに
// service(0) を呼ぶ旧構成は多 peer で O(events×peers) だった。
static int drain_events(ENetHost *host, const client_config *cfg,
                        bk_metrics *metrics,
                        int *connected_count, client_stats *stats) {
  ENetEvent event;
  int rc = enet_host_service(host, &event, 0);
  if (rc < 0) {
    return -1;
  }
  int handled = 0;
  while (rc > 0) {
    if (handle_event(&event, cfg, metrics, connected_count, stats) != 0) {
      return -1;
    }
    if (++handled >= ENET_DRAIN_EVENT_BUDGET) {
      // budget 到達: 残りは dispatch queue に置いたまま送信側に制御を返す
      return 0;
    }
    rc = enet_host_check_events(host, &event);
    if (rc == 0) {
      rc = enet_host_service(host, &event, 0);
    }
    if (rc < 0) {
      return -1;
    }
  }
  return 0;
}

static int wait_for_connects(ENetHost *host, const client_config *cfg,
                             int conns, bk_metrics *metrics,
                             client_stats *stats) {
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
    if (handle_event(&event, cfg, metrics, &connected_count, stats) != 0) {
      return -1;
    }
  }
  return 0;
}

static int send_registrations(ENetHost *host, client_conn *conns,
                              int n_conns) {
  for (int i = 0; i < n_conns; ++i) {
    const bk_header registration = {
        .seq = 0,
        .sched_ts_ns = 0,
        .send_ts_ns = bk_now_ns(),
        .flags = 0,
        .origin_id = conns[i].origin_id,
        .traffic_id = 0,
    };
    ENetPacket *packet = enet_packet_create(
        NULL, BK_MIN_PAYLOAD, ENET_PACKET_FLAG_UNSEQUENCED);
    if (packet == NULL ||
        bk_payload_write(packet->data, packet->dataLength, &registration) != 0 ||
        enet_peer_send(conns[i].peer, ENET_CHANNEL_UNRELIABLE, packet) != 0) {
      if (packet != NULL && packet->referenceCount == 0) {
        enet_packet_destroy(packet);
      }
      return -1;
    }
  }
  enet_host_flush(host);
  return 0;
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
                     size_t payload_size, bk_metrics *metrics) {
  bk_header header;
  make_header_from_slot(slot, conn->origin_id, bk_now_ns(), &header);
  enet_uint8 channel = 0;
  enet_uint32 packet_flags = 0;
  enet_class_route(header.flags, &channel, &packet_flags);
  // data=NULL で packet を確保し、header と deterministic body pattern を
  // packet バッファへ直接書く。中間バッファ経由の payload 全体 memcpy を避ける。
  ENetPacket *packet = enet_packet_create(NULL, payload_size, packet_flags);
  if (packet == NULL ||
      bk_payload_write(packet->data, payload_size, &header) != 0 ||
      bk_payload_fill_body(packet->data, payload_size, &header) != 0) {
    if (packet != NULL) {
      enet_packet_destroy(packet);
    }
    bk_metrics_on_slot(metrics, &header, false);
    return -1;
  }
  const bool submitted = enet_peer_send(conn->peer, channel, packet) == 0;
  if (!submitted) {
    enet_packet_destroy(packet);
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

static int format_client_stats_json(const client_config *cfg,
                                    const client_conn *conns,
                                    const client_stats *stats, char *buf,
                                    size_t cap) {
  bk_authoritative_progress progress = {0};
  if (cfg->scenario.kind == BK_SCENARIO_AUTHORITATIVE_STATE) {
    progress.local_conns = (uint32_t)cfg->conns;
    progress.roster_conns = cfg->scenario.total_conns;
    for (int i = 0; i < cfg->conns; ++i) {
      const client_conn *conn = &conns[i];
      if (i == 0 || conn->input_last_sent_measured <
                        progress.input_last_sent_min) {
        progress.input_last_sent_min = conn->input_last_sent_measured;
      }
      if (conn->input_last_sent_measured > progress.input_last_sent_max) {
        progress.input_last_sent_max = conn->input_last_sent_measured;
      }
      if (i == 0 || conn->state_header_seq_recv_measured <
                        progress.state_header_seq_recv_min) {
        progress.state_header_seq_recv_min =
            conn->state_header_seq_recv_measured;
      }
      if (conn->state_header_seq_recv_measured >
          progress.state_header_seq_recv_max) {
        progress.state_header_seq_recv_max =
            conn->state_header_seq_recv_measured;
      }
      if (i == 0 || conn->state_applied_input_seq_recv_measured <
                        progress.state_applied_input_seq_recv_min) {
        progress.state_applied_input_seq_recv_min =
            conn->state_applied_input_seq_recv_measured;
      }
      if (conn->state_applied_input_seq_recv_measured >
          progress.state_applied_input_seq_recv_max) {
        progress.state_applied_input_seq_recv_max =
            conn->state_applied_input_seq_recv_measured;
      }
    }
  }
  char progress_json[768];
  if (bk_authoritative_progress_format("client", &progress, progress_json,
                                       sizeof(progress_json)) != 0) {
    return -1;
  }
  const int n = snprintf(buf, cap, "{\"invalid_payload\":%" PRIu64 ",%s}",
                         stats->invalid_payload, progress_json);
  return n > 0 && (size_t)n < cap ? 0 : -1;
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

static int expect_scenario_flows(const client_config *cfg,
                                 const client_conn *conns,
                                 bk_metrics *metrics,
                                 const bk_schedule *schedule) {
  if (!cfg->scenario.present) {
    return 0;
  }
  if (cfg->scenario.kind == BK_SCENARIO_AUTHORITATIVE_STATE) {
    if (cfg->scenario.state.rate_lt == 0.0) {
      return 0;
    }
    for (int i = 0; i < cfg->conns; ++i) {
      if (bk_metrics_expect_latest(
              metrics, conns[i].local_index, cfg->scenario.total_conns,
              cfg->scenario.state.traffic_id,
              BK_DIRECTION_SERVER_TO_CLIENT, schedule->start_at_ns) != 0) {
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
        cfg->scenario.kind == BK_SCENARIO_ROOM_RELAY ? 0u
                                                     : conns[i].origin_id;
    const uint32_t end_origin =
        cfg->scenario.kind == BK_SCENARIO_ROOM_RELAY
            ? cfg->scenario.total_conns
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
  const bool authoritative =
      cfg->scenario.kind == BK_SCENARIO_AUTHORITATIVE_STATE;
  const uint32_t max_origin_id =
      cfg->scenario.present ? cfg->scenario.total_conns + 1u
                            : cfg->origin_base + (uint32_t)cfg->conns;
  const bk_metrics_config metrics_cfg = {
      .max_origin_id = max_origin_id == 0 ? 1u : max_origin_id,
      .deadline_ns = cfg->deadline_ns,
      .staleness_period_ns = cfg->staleness_period_ns,
      .max_local_index = cfg->scenario.present ? (uint32_t)cfg->conns : 0,
  };
  bk_metrics *metrics = bk_metrics_new(&metrics_cfg);
  client_stats stats = {0};
  if (metrics == NULL) {
    fprintf(stderr, "bk_metrics_new failed\n");
    return -1;
  }
  if (authoritative &&
      bk_metrics_set_traffic_deadline(
          metrics, cfg->scenario.state.traffic_id,
          BK_DIRECTION_SERVER_TO_CLIENT,
          cfg->scenario.state.deadline_ns) != 0) {
    bk_metrics_free(metrics);
    return -1;
  }

  ENetHost *host = enet_host_create(NULL, (size_t)cfg->conns, 2, 0, 0);
  if (host == NULL) {
    fprintf(stderr, "enet_host_create client failed\n");
    bk_metrics_free(metrics);
    return -1;
  }

  // 計測器(client farm)側の受信バッファ。broadcast fanout の高 pps 受信で
  // kernel 既定 rcvbuf が溢れると server の delivery が過小観測される。
  // 送信側も ENet 既定 256KB(enet.h:213-214)では burst 送信で詰まる
  enet_socket_set_option(host->socket, ENET_SOCKOPT_RCVBUF,
                         ENET_BENCH_SOCKBUF_BYTES);
  enet_socket_set_option(host->socket, ENET_SOCKOPT_SNDBUF,
                         ENET_BENCH_SOCKBUF_BYTES);
  {
    int effective = 0;
    socklen_t optlen = sizeof(effective);
    if (getsockopt(host->socket, SOL_SOCKET, SO_RCVBUF, &effective, &optlen) == 0) {
      fprintf(stderr, "client rcvbuf effective=%d\n", effective);
    }
  }

  client_conn *conns = (client_conn *)calloc((size_t)cfg->conns, sizeof(*conns));
  if (conns == NULL) {
    fprintf(stderr, "allocation failed\n");
    enet_host_destroy(host);
    bk_metrics_free(metrics);
    return -1;
  }

  bk_control *control = bk_control_connect(NULL);
  if (control != NULL &&
      bk_control_hello(control, "client", "enet", cfg->proc_index) != 0) {
    fprintf(stderr, "benchkit hello failed\n");
    bk_control_close(control);
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
    free(conns);
    enet_host_destroy(host);
    bk_metrics_free(metrics);
    return -1;
  }
  address.port = cfg->port;

  for (int i = 0; i < cfg->conns; ++i) {
    conns[i].origin_id = cfg->origin_base + (uint32_t)i;
    conns[i].local_index = (uint32_t)i;
    conns[i].peer = enet_host_connect(host, &address, 2, 0);
    if (conns[i].peer == NULL) {
      fprintf(stderr, "enet_host_connect failed\n");
      if (control != NULL) {
        bk_control_close(control);
      }
      free(conns);
      enet_host_destroy(host);
      bk_metrics_free(metrics);
      return -1;
    }
    conns[i].peer->data = &conns[i];
  }

  if (wait_for_connects(host, cfg, cfg->conns, metrics, &stats) != 0) {
    fprintf(stderr, "timed out waiting for ENet connects\n");
    if (control != NULL) {
      bk_control_close(control);
    }
    free(conns);
    enet_host_destroy(host);
    bk_metrics_free(metrics);
    return -1;
  }
  if (send_registrations(host, conns, cfg->conns) != 0) {
    fprintf(stderr, "ENet registration send failed\n");
    if (control != NULL) {
      bk_control_close(control);
    }
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
      free(conns);
      enet_host_destroy(host);
      bk_metrics_free(metrics);
      return -1;
    }
  }
  if (expect_scenario_flows(cfg, conns, metrics, &schedule) != 0) {
    fprintf(stderr, "bk_metrics_expect_latest failed\n");
    if (control != NULL) {
      bk_control_close(control);
    }
    for (int j = 0; j < cfg->conns; ++j) {
      bk_plan_free(conns[j].plan);
    }
    free(conns);
    enet_host_destroy(host);
    bk_metrics_free(metrics);
    return -1;
  }

  bool marked_unsent = false;
  int connected_count = cfg->conns;
  int run_rc = 0;
  bk_steady steady = {0, false};
  while (bk_now_ns() < schedule.drain_until_ns) {
    uint64_t now = bk_now_ns();
    if (drain_events(host, cfg, metrics, &connected_count, &stats) != 0) {
      fprintf(stderr, "enet event handling failed\n");
      run_rc = -1;
      break;
    }
    // 定常判定つき warmup(benchspec v2): rate 報告と確定窓(window)の受信。
    // window を受けたら全 conn の plan に計測窓を差し替える
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
          const size_t payload_size = (slot.flags & BK_FLAG_MUST_DELIVER)
                                          ? cfg->payload_md
                                          : cfg->payload_lt;
          if (send_slot(&conns[i], &slot, payload_size, metrics) == 0) {
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
    if (rc > 0 &&
        handle_event(&event, cfg, metrics, &connected_count, &stats) != 0) {
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

  char stats_json[1024];
  if (format_client_stats_json(cfg, conns, &stats, stats_json,
                               sizeof(stats_json)) != 0) {
    fprintf(stderr, "client stats JSON overflow\n");
    rc = -1;
  }
  if (control != NULL) {
    if (rc == 0 && bk_control_done(control, stats_json) != 0) {
      fprintf(stderr, "benchkit done failed\n");
      rc = -1;
    }
    bk_control_close(control);
  }

  for (int i = 0; i < cfg->conns; ++i) {
    bk_plan_free(conns[i].plan);
  }
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
