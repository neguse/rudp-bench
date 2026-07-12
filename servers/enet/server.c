#include "benchkit.h"
#include "../scenario_cli.h"
#include "../ramp.h"

#include <enet/enet.h>

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ENet promotes oversized UNSEQUENCED packets to reliable fragmentation.
// Use the conservative common-class ceiling advertised by the client.
#define ENET_BENCH_MAX_PAYLOAD_BYTES 1000u
#define ENET_BENCH_MAX_PEERS 4095u
#define ENET_CHANNEL_RELIABLE 0u
#define ENET_CHANNEL_UNRELIABLE 1u
#define ENET_SERVICE_SLICE_MS 10u
// 1 回の service_once で処理するイベント数の上限。無制限だと持続的な受信で
// ループから抜けられず、main ループの制御チャネル poll が飢える(raw_udp の
// drain 上限と同じ理由)。超過分は dispatch queue に残り次呼び出しで続きを引く。
#define ENET_SERVICE_EVENT_BUDGET 4096
#define ENET_BENCH_SOCKBUF_BYTES (4 * 1024 * 1024)
#define DEV_WARMUP_NS 200000000ull
#define DEV_DURATION_NS 2000000000ull
#define DEV_DRAIN_NS 500000000ull

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
  uint64_t server_state_ticks;
} server_stats;

typedef struct {
  uint16_t port;
  bk_scenario_cli scenario;
  uint64_t staleness_period_ns;
} server_config;

typedef struct {
  bool registered;
  uint32_t origin_id;
  uint64_t applied_input_seq;
} peer_state;

typedef struct {
  ENetHost *host;
  const server_config *cfg;
  server_stats *stats;
  bk_metrics *metrics;
  peer_state *peer_states;
  ENetPeer **roster;
  uint32_t registered_count;
  bool roster_frozen;
} server_context;

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int signo) {
  (void)signo;
  g_stop = 1;
}

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
       "\"payload_pattern\":\"splitmix64-v1\","
       "\"wire_compression\":\"none\","
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
       "{\"knob\":\"forwarding\","
       "\"value\":\"shared ENetPacket reference\","
       "\"upstream_ref\":\"https://github.com/lsalzman/enet/blob/master/include/enet/enet.h\"},"
       "{\"knob\":\"event_drain\","
       "\"value\":\"enet_host_check_events\","
       "\"upstream_ref\":\"https://github.com/lsalzman/enet/blob/master/include/enet/enet.h\"}]}");
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

static int parse_args(int argc, char **argv, server_config *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->staleness_period_ns = 10000000ull;
  bool have_port = false;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--describe") == 0) {
      print_describe();
      exit(EXIT_SUCCESS);
    }
    if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      if (parse_u16(argv[++i], &cfg->port) != 0 || cfg->port == 0) {
        return -1;
      }
      have_port = true;
      continue;
    }
    if (strcmp(argv[i], "--staleness-period-ns") == 0 && i + 1 < argc) {
      if (bk_scenario_parse_u64(argv[++i], &cfg->staleness_period_ns) != 0 ||
          cfg->staleness_period_ns == 0) {
        return -1;
      }
      continue;
    }
    const int parsed =
        bk_scenario_cli_parse(argc, argv, &i, &cfg->scenario);
    if (parsed < 0) {
      return -1;
    }
    if (parsed > 0) {
      continue;
    }
    return -1;
  }
  if (!have_port ||
      bk_scenario_cli_validate(&cfg->scenario, ENET_BENCH_MAX_PEERS,
                               ENET_BENCH_MAX_PAYLOAD_BYTES) != 0) {
    return -1;
  }
  return 0;
}

static class_index class_from_flags(uint8_t flags) {
  return (flags & BK_FLAG_MUST_DELIVER) != 0 ? CLASS_MUST_DELIVER
                                             : CLASS_LOSS_TOLERANT;
}

static dist_index dist_from_flags(uint8_t flags) {
  return (flags & BK_FLAG_BROADCAST) != 0 ? DIST_BROADCAST : DIST_ECHO;
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

// peer 単位チューニング(upstream 公式 API。--describe の tuning に開示):
// - packet throttle: 減速を無効化。既定(accel=2,decel=2)は burst loss で
//   throttle が 32→0 に滑落し、unreliable の大半をライブラリが送信キューで
//   自己破棄する(docs/ledger.md #11)。throttle は reliable window の実効幅
//   (packetThrottle*windowSize/32, protocol.c:1470)にも掛かる
// - timeout: service が高負荷で間引かれると reliable timeout
//   (既定 min5s/max30s)で切断される(ledger #8)。検出遅延と引き換えに延長
static void tune_peer(ENetPeer *peer) {
  enet_peer_throttle_configure(peer, ENET_PEER_PACKET_THROTTLE_INTERVAL,
                               ENET_PEER_PACKET_THROTTLE_SCALE, 0);
  enet_peer_timeout(peer, ENET_PEER_TIMEOUT_LIMIT, 10000, 60000);
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

// 受信 packet の所有権はこの関数が引き取る(転送成功なら ENet の refcount
// 管理に移り、失敗・不正 payload ならここで destroy する)。
static int register_origin(server_context *ctx, ENetPeer *peer,
                           uint32_t origin_id) {
  if (ctx->cfg->scenario.kind != BK_SCENARIO_AUTHORITATIVE_STATE) {
    return 0;
  }
  if (origin_id >= ctx->cfg->scenario.total_conns) {
    return -1;
  }
  const size_t peer_index = (size_t)(peer - ctx->host->peers);
  peer_state *state = &ctx->peer_states[peer_index];
  if (state->registered) {
    return state->origin_id == origin_id ? 0 : -1;
  }
  if (ctx->roster_frozen || ctx->roster[origin_id] != NULL) {
    return -1;
  }
  state->registered = true;
  state->origin_id = origin_id;
  state->applied_input_seq = 0;
  ctx->roster[origin_id] = peer;
  ctx->registered_count++;
  return 0;
}

static void handle_disconnect(server_context *ctx, ENetPeer *peer) {
  if (ctx->cfg->scenario.kind != BK_SCENARIO_AUTHORITATIVE_STATE) {
    return;
  }
  const size_t peer_index = (size_t)(peer - ctx->host->peers);
  peer_state *state = &ctx->peer_states[peer_index];
  if (state->registered) {
    ctx->roster[state->origin_id] = NULL;
    state->registered = false;
    if (!ctx->roster_frozen) {
      ctx->registered_count--;
    }
  }
}

static void handle_receive(server_context *ctx, const ENetEvent *event) {
  ENetPacket *packet = event->packet;
  bk_header header;
  if (bk_payload_read(packet->data, packet->dataLength, &header) != 0) {
    ctx->stats->invalid_payload++;
    enet_packet_destroy(packet);
    return;
  }
  if (!route_matches_header(event, &header)) {
    ctx->stats->invalid_payload++;
    enet_packet_destroy(packet);
    return;
  }

  const bool registration =
      bk_payload_is_registration(&header, packet->dataLength) != 0;
  if (registration) {
    if (ctx->cfg->scenario.present &&
        header.origin_id >= ctx->cfg->scenario.total_conns) {
      ctx->stats->invalid_payload++;
      enet_packet_destroy(packet);
      return;
    }
    if (ctx->cfg->scenario.kind == BK_SCENARIO_AUTHORITATIVE_STATE &&
        register_origin(ctx, event->peer, header.origin_id) != 0) {
      ctx->stats->invalid_payload++;
      enet_packet_destroy(packet);
      return;
    }
    enet_packet_destroy(packet);
    return;
  }

  if (ctx->cfg->scenario.present &&
      (!bk_scenario_client_payload_valid(&ctx->cfg->scenario, &header,
                                         packet->dataLength) ||
       bk_payload_validate_body(packet->data, packet->dataLength, &header) !=
           0)) {
    ctx->stats->invalid_payload++;
    enet_packet_destroy(packet);
    return;
  }
  if (!ctx->cfg->scenario.present &&
      bk_payload_validate_body(packet->data, packet->dataLength, &header) != 0) {
    ctx->stats->invalid_payload++;
    enet_packet_destroy(packet);
    return;
  }

  if (ctx->cfg->scenario.kind == BK_SCENARIO_AUTHORITATIVE_STATE) {
    if (register_origin(ctx, event->peer, header.origin_id) != 0) {
      ctx->stats->invalid_payload++;
      enet_packet_destroy(packet);
      return;
    }
    const size_t peer_index = (size_t)(event->peer - ctx->host->peers);
    peer_state *state = &ctx->peer_states[peer_index];
    if ((header.flags & BK_FLAG_MUST_DELIVER) == 0 &&
        header.seq > state->applied_input_seq) {
      state->applied_input_seq = header.seq;
    }
    bk_metrics_on_recv(ctx->metrics, header.origin_id, &header, bk_now_ns());
    enet_packet_destroy(packet);
    return;
  }

  const class_index cls = class_from_flags(header.flags);
  const dist_index dist = dist_from_flags(header.flags);
  const bool measured = (header.flags & BK_FLAG_MEASURE) != 0;
  ctx->stats->recv[cls][dist]++;
  if (measured) {
    ctx->stats->recv_measured[cls][dist]++;
  }

  // 受信 packet をそのまま転送する(zero-copy)。受信経路で class 相当の
  // packet flag(RELIABLE / UNSEQUENCED)が付与済みなので(protocol.c:462,506)
  // 再 create せず forward しても class mapping は保存される。
  const enet_uint8 channel = channel_from_flags(header.flags);

  if (dist == DIST_ECHO) {
    const bool ok = enet_peer_send(event->peer, channel, packet) == 0;
    count_submit(ctx->stats, cls, dist, measured, ok);
    if (!ok) {
      enet_packet_destroy(packet);
    }
    return;
  }

  // broadcast: 1 packet を全 peer で refcount 共有(enet_host_broadcast と
  // 同方式、host.c:271-288)。per-peer の malloc+copy を発生させない。
  // per-peer の submit 会計が要るため enet_host_broadcast は使わず手で回す。
  for (size_t i = 0; i < ctx->host->peerCount; ++i) {
    ENetPeer *peer = &ctx->host->peers[i];
    if (peer->state != ENET_PEER_STATE_CONNECTED) {
      continue;
    }
    const bool ok = enet_peer_send(peer, channel, packet) == 0;
    count_submit(ctx->stats, cls, dist, measured, ok);
  }
  if (packet->referenceCount == 0) {
    enet_packet_destroy(packet);
  }
}

// イベント処理ループ。dispatch 済みイベントは enet_host_check_events で
// I/O なしに引く。enet_host_service は 1 呼び出しで最大 1 event しか返さず、
// 毎回 全 peer 走査の送信パス×2 + 受信を回すため(protocol.c:1830,1846,1862)、
// event ごとに service(0) を呼ぶ旧構成は多 peer で O(events×peers) だった。
static int service_once(server_context *ctx, enet_uint32 timeout_ms) {
  ENetEvent event;
  int rc = enet_host_service(ctx->host, &event, timeout_ms);
  if (rc < 0) {
    return -1;
  }
  // budget はイベント数でなく仕事量で bound する: broadcast イベント 1 件の
  // 仕事は O(接続数) なので、固定イベント数だと高 conns で 1 呼び出しが
  // budget×conns 送信に膨らみ、main ループの制御チャネル poll が飢える
  // (raw_udp server と同じ negative window margin、ledger #20 と同族)
  const int fanout =
      ctx->host->connectedPeers > 0 ? (int)ctx->host->connectedPeers : 1;
  int budget = ENET_SERVICE_EVENT_BUDGET / fanout;
  if (budget < 64) {
    budget = 64;
  }
  int handled = 0;
  while (rc > 0) {
    switch (event.type) {
      case ENET_EVENT_TYPE_CONNECT:
        tune_peer(event.peer);
        break;
      case ENET_EVENT_TYPE_RECEIVE:
        handle_receive(ctx, &event);
        break;
      case ENET_EVENT_TYPE_DISCONNECT:
        handle_disconnect(ctx, event.peer);
        event.peer->data = NULL;
        break;
      case ENET_EVENT_TYPE_NONE:
        break;
    }

    if (++handled >= budget) {
      // budget 到達で中断: 積んだ応答だけ吐いて制御チャネルに戻る
      enet_host_flush(ctx->host);
      return 0;
    }
    rc = enet_host_check_events(ctx->host, &event);
    if (rc == 0) {
      // dispatch queue が空になったら service(0) で送信+受信をもう 1 パス。
      // このパスが直前までに積んだ応答を flush する
      rc = enet_host_service(ctx->host, &event, 0);
    }
    if (rc < 0) {
      return -1;
    }
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
  *out = (uint64_t)(interval + 0.5);
  if (*out == 0) {
    *out = 1;
  }
  return 0;
}

static int build_state_streams(const server_config *cfg, bk_stream *streams,
                               int *n_streams) {
  const bk_scenario_traffic *state = &cfg->scenario.state;
  int n = 0;
  uint64_t interval_ns = 0;
  if (state->rate_lt > 0.0) {
    if (interval_from_rate(state->rate_lt, &interval_ns) != 0) {
      return -1;
    }
    streams[n++] = (bk_stream){
        .must_deliver = false,
        .broadcast = false,
        .traffic_id = state->traffic_id,
        .direction = BK_DIRECTION_SERVER_TO_CLIENT,
        .interval_ns = interval_ns,
    };
  }
  if (state->rate_md > 0.0) {
    if (interval_from_rate(state->rate_md, &interval_ns) != 0) {
      return -1;
    }
    streams[n++] = (bk_stream){
        .must_deliver = true,
        .broadcast = false,
        .traffic_id = state->traffic_id,
        .direction = BK_DIRECTION_SERVER_TO_CLIENT,
        .interval_ns = interval_ns,
    };
  }
  *n_streams = n;
  return n == 0 ? -1 : 0;
}

static void make_state_header(const server_config *cfg, const bk_slot *slot,
                              uint64_t send_ts_ns, bk_header *header) {
  *header = (bk_header){
      .seq = slot->seq,
      .sched_ts_ns = slot->sched_ts_ns,
      .send_ts_ns = send_ts_ns,
      .flags = (uint8_t)(slot->flags & (uint8_t)~BK_FLAG_BROADCAST),
      .origin_id = cfg->scenario.total_conns,
      .traffic_id = slot->traffic_id,
  };
}

static size_t state_payload_size(const server_config *cfg,
                                 const bk_slot *slot) {
  return (slot->flags & BK_FLAG_MUST_DELIVER) != 0
             ? cfg->scenario.state.payload_md
             : cfg->scenario.state.payload_lt;
}

static void send_state_slot(server_context *ctx, const bk_slot *slot,
                            uint32_t ramp_target_conns) {
  if ((slot->flags & (BK_FLAG_MEASURE | BK_FLAG_MUST_DELIVER)) ==
      BK_FLAG_MEASURE) {
    ctx->stats->server_state_ticks++;
  }
  const size_t payload_size = state_payload_size(ctx->cfg, slot);
  const enet_uint8 channel = channel_from_flags(slot->flags);
  const enet_uint32 packet_flags =
      (slot->flags & BK_FLAG_MUST_DELIVER) != 0
          ? ENET_PACKET_FLAG_RELIABLE
          : ENET_PACKET_FLAG_UNSEQUENCED;
  const uint32_t targets = ramp_target_conns != 0
                               ? ramp_target_conns
                               : ctx->cfg->scenario.total_conns;
  for (uint32_t target = 0; target < targets; ++target) {
    ENetPeer *peer = ctx->roster[target];
    bk_header header;
    make_state_header(ctx->cfg, slot, 0, &header);
    bool submitted = false;
    ENetPacket *packet = NULL;
    if (peer != NULL && peer->state == ENET_PEER_STATE_CONNECTED) {
      const size_t peer_index = (size_t)(peer - ctx->host->peers);
      packet = enet_packet_create(NULL, payload_size, packet_flags);
      if (packet != NULL &&
          bk_authoritative_state_write_applied_input_seq(
              packet->data, payload_size,
              ctx->peer_states[peer_index].applied_input_seq) == 0 &&
          bk_authoritative_state_fill_target_pad(packet->data, payload_size,
                                                  target) == 0) {
        header.send_ts_ns = bk_now_ns();
        if (bk_payload_write(packet->data, payload_size, &header) == 0) {
          submitted = enet_peer_send(peer, channel, packet) == 0;
        }
      }
    }
    if (!submitted && packet != NULL && packet->referenceCount == 0) {
      enet_packet_destroy(packet);
    }
    bk_metrics_on_slot(ctx->metrics, &header, submitted);
  }
  enet_host_flush(ctx->host);
}

static void mark_state_unsent(server_context *ctx, bk_plan *plan,
                              uint64_t cutoff_ns, uint32_t target_conns) {
  bk_slot slot;
  while (bk_plan_next(plan, cutoff_ns, &slot)) {
    if ((slot.flags & (BK_FLAG_MEASURE | BK_FLAG_MUST_DELIVER)) ==
        BK_FLAG_MEASURE) {
      ctx->stats->server_state_ticks++;
    }
    for (uint32_t target = 0; target < target_conns; ++target) {
      (void)target;
      bk_header header;
      make_state_header(ctx->cfg, &slot, 0, &header);
      bk_metrics_on_slot(ctx->metrics, &header, false);
    }
  }
}

static int expect_client_inputs(server_context *ctx,
                                const bk_schedule *schedule) {
  if (ctx->cfg->scenario.input.rate_lt == 0.0) {
    return 0;
  }
  for (uint32_t origin = 0; origin < ctx->cfg->scenario.total_conns;
       ++origin) {
    if (bk_metrics_expect_latest(
            ctx->metrics, origin, origin,
            ctx->cfg->scenario.input.traffic_id,
            BK_DIRECTION_CLIENT_TO_SERVER, schedule->start_at_ns) != 0) {
      return -1;
    }
  }
  return 0;
}

static int expect_target_client_inputs(server_context *ctx,
                                       uint32_t target_conns,
                                       uint64_t first_sched_ts_ns) {
  if (ctx->cfg->scenario.input.rate_lt == 0.0) {
    return 0;
  }
  for (uint32_t origin = 0; origin < target_conns; ++origin) {
    if (bk_metrics_expect_latest(
            ctx->metrics, origin, origin,
            ctx->cfg->scenario.input.traffic_id,
            BK_DIRECTION_CLIENT_TO_SERVER, first_sched_ts_ns) != 0) {
      return -1;
    }
  }
  return 0;
}

static int format_stats_json(const server_stats *s, const server_config *cfg,
                             const server_context *ctx, char *buf,
                             size_t cap) {
  bk_authoritative_progress progress = {0};
  if (cfg->scenario.kind == BK_SCENARIO_AUTHORITATIVE_STATE) {
    progress.roster_conns = ctx->registered_count;
    progress.server_state_ticks = s->server_state_ticks;
  }
  char progress_json[768];
  if (bk_authoritative_progress_format("server", &progress, progress_json,
                                       sizeof(progress_json)) != 0) {
    return -1;
  }
  const int prefix = snprintf(
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
      ",\"broadcast\":%" PRIu64 "}},\"invalid_payload\":%" PRIu64 ",",
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
  if (prefix <= 0 || (size_t)prefix >= cap) {
    return -1;
  }
  const int suffix = snprintf(buf + prefix, cap - (size_t)prefix, "%s}",
                              progress_json);
  return suffix > 0 && (size_t)suffix < cap - (size_t)prefix ? 0 : -1;
}

static int write_metrics_out(const bk_metrics *metrics) {
  const char *path = getenv("BENCH_METRICS_OUT");
  if (path == NULL || *path == '\0') {
    return 0;
  }
  return bk_metrics_dump_json(metrics, path);
}

int main(int argc, char **argv) {
  server_config cfg;
  if (parse_args(argc, argv, &cfg) != 0) {
    usage(argv[0]);
    return EXIT_FAILURE;
  }
  bk_ramp_config ramp;
  if (bk_ramp_config_load(cfg.scenario.total_conns, &ramp) != 0) {
    fprintf(stderr, "invalid BENCH_RAMP_* configuration\n");
    return EXIT_FAILURE;
  }

  if (signal(SIGINT, on_signal) == SIG_ERR ||
      signal(SIGTERM, on_signal) == SIG_ERR) {
    perror("signal");
    return EXIT_FAILURE;
  }

  if (enet_initialize() != 0) {
    fprintf(stderr, "enet_initialize failed\n");
    return EXIT_FAILURE;
  }
  atexit(enet_deinitialize);

  ENetAddress address;
  memset(&address, 0, sizeof(address));
  address.host = ENET_HOST_ANY;
  address.port = cfg.port;

  const size_t host_peer_count =
      ramp.enabled ? (size_t)cfg.scenario.total_conns
                   : (size_t)ENET_BENCH_MAX_PEERS;
  ENetHost *host = enet_host_create(&address, host_peer_count, 2, 0, 0);
  if (host == NULL) {
    fprintf(stderr, "enet_host_create server failed on port %" PRIu16 "\n",
            cfg.port);
    return EXIT_FAILURE;
  }
  // 全 conn の集約トラフィックを 1 socket で受ける。ENet 既定 256KB
  // (enet.h:213-214)は broadcast fanout の burst で溢れる
  enet_socket_set_option(host->socket, ENET_SOCKOPT_RCVBUF,
                         ENET_BENCH_SOCKBUF_BYTES);
  enet_socket_set_option(host->socket, ENET_SOCKOPT_SNDBUF,
                         ENET_BENCH_SOCKBUF_BYTES);

  server_stats stats;
  memset(&stats, 0, sizeof(stats));

  const bool authoritative =
      cfg.scenario.kind == BK_SCENARIO_AUTHORITATIVE_STATE;
  const uint32_t max_origin_id =
      cfg.scenario.present ? cfg.scenario.total_conns + 1u : 1u;
  const bk_metrics_config metrics_cfg = {
      .max_origin_id = max_origin_id,
      .deadline_ns = authoritative ? cfg.scenario.input.deadline_ns : 0,
      .staleness_period_ns = cfg.staleness_period_ns,
      .max_local_index = authoritative ? cfg.scenario.total_conns : 0,
  };
  bk_metrics *metrics = bk_metrics_new(&metrics_cfg);
  peer_state *peer_states =
      (peer_state *)calloc(host->peerCount, sizeof(*peer_states));
  ENetPeer **roster =
      authoritative
          ? (ENetPeer **)calloc(cfg.scenario.total_conns, sizeof(*roster))
          : NULL;
  if (metrics == NULL || peer_states == NULL ||
      (authoritative && roster == NULL)) {
    free(roster);
    free(peer_states);
    bk_metrics_free(metrics);
    enet_host_destroy(host);
    return EXIT_FAILURE;
  }
  if (authoritative &&
      (bk_metrics_set_traffic_deadline(
           metrics, cfg.scenario.input.traffic_id,
           BK_DIRECTION_CLIENT_TO_SERVER,
           cfg.scenario.input.deadline_ns) != 0 ||
       bk_metrics_set_traffic_deadline(
           metrics, cfg.scenario.state.traffic_id,
           BK_DIRECTION_SERVER_TO_CLIENT,
           cfg.scenario.state.deadline_ns) != 0)) {
    free(roster);
    free(peer_states);
    bk_metrics_free(metrics);
    enet_host_destroy(host);
    return EXIT_FAILURE;
  }
  server_context ctx = {
      .host = host,
      .cfg = &cfg,
      .stats = &stats,
      .metrics = metrics,
      .peer_states = peer_states,
      .roster = roster,
      .registered_count = 0,
      .roster_frozen = false,
  };

  bk_control *control = bk_control_connect(NULL);
  bk_schedule schedule = {0, 0, 0};
  if (control != NULL) {
    if (bk_control_hello(control, "server", "enet", 0) != 0 ||
        bk_control_ready(control, 0) != 0) {
      fprintf(stderr, "benchkit control handshake failed\n");
      bk_control_close(control);
      enet_host_destroy(host);
      return EXIT_FAILURE;
    }
    // schedule 待ちの間も service を回す。ここでブロックすると client の
    // 接続ハンドシェイクに応答できず、barrier 全体がデッドロックする。
    for (;;) {
      const int r = bk_control_poll_schedule(control, &schedule);
      if (r == 1) {
        break;
      }
      if (r < 0 || g_stop) {
        fprintf(stderr, "benchkit schedule wait failed\n");
        bk_control_close(control);
        enet_host_destroy(host);
        return EXIT_FAILURE;
      }
      if (service_once(&ctx, ENET_SERVICE_SLICE_MS) != 0) {
        fprintf(stderr, "enet_host_service failed\n");
        bk_control_close(control);
        enet_host_destroy(host);
        return EXIT_FAILURE;
      }
    }
  }

  bool schedule_valid = control != NULL;
  bool window_final = false;
  bool state_marked_unsent = false;
  int run_rc = 0;
  bk_plan *state_plan = NULL;
  bk_ramp_phase ramp_phase;
  if (ramp.enabled) {
    bk_ramp_phase_begin(&ramp, schedule.start_at_ns, &ramp_phase);
  }
  while (!g_stop) {
    uint64_t now = bk_now_ns();
    if (ramp.enabled && bk_ramp_stop_requested()) {
      break;
    }
    if (authoritative && !ramp.enabled && !schedule_valid &&
        ctx.registered_count == cfg.scenario.total_conns) {
      schedule.start_at_ns = add_ns(now, DEV_WARMUP_NS);
      schedule.stop_at_ns = add_ns(schedule.start_at_ns, DEV_DURATION_NS);
      schedule.drain_until_ns = add_ns(schedule.stop_at_ns, DEV_DRAIN_NS);
      schedule_valid = true;
    }
    if (authoritative && !ramp.enabled && schedule_valid &&
        !ctx.roster_frozen &&
        now >= schedule.start_at_ns) {
      fprintf(stderr,
              "authoritative roster incomplete before measurement start\n");
      run_rc = -1;
      break;
    }
    if (authoritative && !ramp.enabled && schedule_valid &&
        !ctx.roster_frozen &&
        ctx.registered_count == cfg.scenario.total_conns) {
      bk_stream streams[2];
      int n_streams = 0;
      if (build_state_streams(&cfg, streams, &n_streams) != 0) {
        run_rc = -1;
        break;
      }
      state_plan = bk_plan_new(streams, n_streams, now,
                               schedule.start_at_ns, schedule.stop_at_ns);
      if (state_plan == NULL || expect_client_inputs(&ctx, &schedule) != 0) {
        fprintf(stderr, "authoritative state initialization failed\n");
        run_rc = -1;
        break;
      }
      ctx.roster_frozen = true;
    }

    if (authoritative && ramp.enabled && schedule_valid &&
        state_plan == NULL) {
      bk_stream streams[2];
      int n_streams = 0;
      if (build_state_streams(&cfg, streams, &n_streams) != 0) {
        run_rc = -1;
        break;
      }
      state_plan = bk_plan_new(streams, n_streams, now,
                               schedule.start_at_ns, schedule.stop_at_ns);
      if (state_plan == NULL) {
        fprintf(stderr, "ramp authoritative state initialization failed\n");
        run_rc = -1;
        break;
      }
    }

    if (ramp.enabled && now >= ramp_phase.phase_start_ns) {
      if (!ramp_phase.reset_done && now >= ramp_phase.phase_end_ns) {
        fprintf(stderr, "ramp phase stalled across reset and phase end\n");
        run_rc = -1;
        break;
      }
      // Install the phase accounting window at phase start. The cohort still
      // starts after guard, so packets serviced during guard are ignored.
      if (!ramp_phase.reset_done) {
        bk_metrics_reset(metrics);
        if (bk_metrics_set_cohort(metrics, ramp_phase.reset_at_ns,
                                  ramp_phase.sample_end_ns) != 0 ||
            (authoritative &&
             expect_target_client_inputs(&ctx, ramp_phase.target_conns,
                                         ramp_phase.reset_at_ns) != 0)) {
          fprintf(stderr, "ramp expected-flow registration failed\n");
          run_rc = -1;
          break;
        }
        ramp_phase.sample_conns = ramp_phase.target_conns;
        ramp_phase.reset_done = 1;
      }
      if (now >= ramp_phase.phase_end_ns) {
        if (state_plan != NULL) {
          mark_state_unsent(&ctx, state_plan,
                            ramp_phase.sample_end_ns,
                            ramp_phase.target_conns);
        }
        if (!ramp_phase.reset_done ||
            bk_ramp_dump_metrics(metrics, ramp_phase.phase,
                                 ramp_phase.sample_conns) != 0) {
          fprintf(stderr, "ramp metrics dump failed\n");
          run_rc = -1;
          break;
        }
        if (bk_ramp_stop_requested()) {
          break;
        }
        if (!bk_ramp_phase_advance(&ramp, cfg.scenario.total_conns,
                                   &ramp_phase)) {
          break;
        }
        continue;
      }
    }

    if (control != NULL && !window_final && !ramp.enabled) {
      if (now >= schedule.start_at_ns) {
        window_final = true;
      } else {
        const int wr = bk_control_poll_window(control, &schedule);
        if (wr < 0) {
          fprintf(stderr, "benchkit window poll failed\n");
          run_rc = -1;
          break;
        }
        if (wr == 1) {
          window_final = true;
          if (state_plan != NULL) {
            bk_plan_set_window(state_plan, schedule.start_at_ns,
                               schedule.stop_at_ns);
          }
        }
      }
    }

    now = bk_now_ns();
    if (schedule_valid && now >= schedule.start_at_ns &&
        now < schedule.stop_at_ns) {
      bk_metrics_tick(metrics, now);
    }
    if (state_plan != NULL && now < schedule.stop_at_ns) {
      bk_slot slot;
      while (bk_plan_next(state_plan, now, &slot)) {
        send_state_slot(&ctx, &slot,
                        ramp.enabled ? ramp_phase.target_conns : 0);
      }
    } else if (state_plan != NULL && !state_marked_unsent) {
      const uint64_t cutoff =
          schedule.stop_at_ns == 0 ? 0 : schedule.stop_at_ns - 1u;
      mark_state_unsent(&ctx, state_plan, cutoff,
                        cfg.scenario.total_conns);
      state_marked_unsent = true;
    }
    if (!ramp.enabled && control != NULL &&
        now >= schedule.drain_until_ns) {
      break;
    }

    enet_uint32 timeout_ms = ENET_SERVICE_SLICE_MS;
    if (state_plan != NULL && now < schedule.stop_at_ns) {
      const uint64_t due = bk_plan_peek_ns(state_plan);
      if (due <= now) {
        timeout_ms = 0;
      } else {
        const uint64_t wait_ms = (due - now) / 1000000ull;
        if (wait_ms < timeout_ms) {
          timeout_ms = (enet_uint32)wait_ms;
        }
      }
    }
    if (ramp.enabled) {
      const uint64_t phase_due = ramp_phase.reset_done
                                     ? ramp_phase.phase_end_ns
                                     : ramp_phase.phase_start_ns;
      if (phase_due <= now) {
        timeout_ms = 0;
      } else {
        const uint64_t wait_ms = (phase_due - now) / 1000000ull;
        if (wait_ms < timeout_ms) {
          timeout_ms = (enet_uint32)wait_ms;
        }
      }
    }
    if (service_once(&ctx, timeout_ms) != 0) {
      fprintf(stderr, "enet_host_service failed\n");
      run_rc = -1;
      break;
    }
  }

  if (!ramp.enabled && state_plan != NULL && !state_marked_unsent) {
    const uint64_t cutoff =
        schedule.stop_at_ns == 0 ? 0 : schedule.stop_at_ns - 1u;
    mark_state_unsent(&ctx, state_plan, cutoff, cfg.scenario.total_conns);
  }

  char stats_json[4096];
  if (format_stats_json(&stats, &cfg, &ctx, stats_json,
                        sizeof(stats_json)) != 0) {
    fprintf(stderr, "server stats JSON overflow\n");
    if (control != NULL) {
      bk_control_close(control);
    }
    enet_host_destroy(host);
    return EXIT_FAILURE;
  }

  if (!ramp.enabled && write_metrics_out(metrics) != 0) {
    fprintf(stderr, "failed to write BENCH_METRICS_OUT\n");
    run_rc = -1;
  }

  if (control != NULL && run_rc == 0) {
    if (bk_control_done(control, stats_json) != 0) {
      fprintf(stderr, "benchkit done failed\n");
      run_rc = -1;
    }
    bk_control_close(control);
    control = NULL;
  }

  if (control != NULL) {
    bk_control_close(control);
  }
  bk_plan_free(state_plan);
  free(roster);
  free(peer_states);
  bk_metrics_free(metrics);
  enet_host_destroy(host);
  return run_rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
