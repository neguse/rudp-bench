#include "benchkit.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
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

// raw_udp is both the v2 room-relay ceiling and the v3 authoritative-state
// reference. It deliberately adds no reliability or congestion control.

#define RAWUDP_MAX_PAYLOAD_BYTES 65507u
#define RAWUDP_SOCKET_BUFFER_BYTES (256 * 1024)
#define RAWUDP_SERVICE_SLICE_MS 10
#define RAWUDP_SERVER_DRAIN_BUDGET 512
#define RAWUDP_MAX_CONNS 65535u
#define DEV_WARMUP_NS 200000000ull
#define DEV_DURATION_NS 2000000000ull
#define DEV_DRAIN_NS 500000000ull

typedef enum {
  CLASS_LOSS_TOLERANT = 0,
  CLASS_MUST_DELIVER = 1,
  CLASS_COUNT = 2,
} class_index;

typedef enum {
  DIST_ECHO = 0,
  DIST_BROADCAST = 1,
  DIST_COUNT = 2,
} dist_index;

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
  uint16_t port;
  scenario_kind scenario;
  uint32_t total_conns;
  uint64_t staleness_period_ns;
  traffic_config input;
  traffic_config state;
  traffic_config publish;
} server_config;

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
  struct sockaddr_in addr;
  uint32_t origin_id;
  uint64_t applied_input_seq;
} peer;

typedef struct {
  peer *peers;
  size_t count;
  size_t cap;
  uint64_t *keys;
  size_t *values;  // peer index + 1; 0 means empty
  size_t hcap;
  size_t *origin_to_peer;  // peer index + 1; authoritative mode only
  size_t origin_cap;
} peer_table;

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int signo) {
  (void)signo;
  g_stop = 1;
}

static void print_describe(void) {
  puts("{\"transport\":\"raw_udp\","
       "\"class_mapping\":{"
       "\"loss_tolerant\":{\"primitive\":\"unreliable-udp\","
       "\"delivery\":\"best_effort\",\"ordering\":\"unordered\","
       "\"realization\":\"native\"},"
       "\"must_deliver\":{\"primitive\":\"unreliable-udp\","
       "\"delivery\":\"best_effort\",\"ordering\":\"unordered\","
       "\"realization\":\"unsupported\"}},"
       "\"coalescing\":\"none\",\"cc_algo\":\"none\","
       "\"thread_model\":\"single\",\"encryption\":false,"
       "\"payload_pattern\":\"splitmix64-v1\","
       "\"wire_compression\":\"none\","
       "\"max_payload_bytes\":65507,"
       "\"scenarios\":[\"environment_baseline\","
       "\"authoritative_state\",\"room_relay\"],\"tuning\":[]}");
}

static void usage(const char *argv0) {
  fprintf(stderr, "usage: %s --port PORT [--scenario KIND ...]\n", argv0);
}

static int parse_u64(const char *s, uint64_t *out) {
  if (s == NULL || *s == '\0') {
    return -1;
  }
  errno = 0;
  char *end = NULL;
  const unsigned long long v = strtoull(s, &end, 10);
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

static int parse_u16(const char *s, uint16_t *out) {
  uint32_t v = 0;
  if (parse_u32(s, &v) != 0 || v > UINT16_MAX) {
    return -1;
  }
  *out = (uint16_t)v;
  return 0;
}

static int parse_size(const char *s, size_t *out) {
  uint64_t v = 0;
  if (parse_u64(s, &v) != 0 || v > SIZE_MAX) {
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
#define MATCH_TRAFFIC_ARG(suffix)                                      \
  (snprintf(name, sizeof(name), "--%s-%s", prefix, suffix) > 0 &&     \
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

static int parse_args(int argc, char **argv, server_config *cfg) {
  memset(cfg, 0, sizeof(*cfg));
  cfg->staleness_period_ns = 10000000ull;
  bool have_port = false;
  bool have_scenario = false;
  bool have_total = false;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--describe") == 0) {
      print_describe();
      exit(EXIT_SUCCESS);
    }
    if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      have_port = parse_u16(argv[++i], &cfg->port) == 0 && cfg->port != 0;
      continue;
    }
    if (strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
      if (parse_scenario(argv[++i], &cfg->scenario) != 0) {
        return -1;
      }
      have_scenario = true;
      continue;
    }
    if (strcmp(argv[i], "--total-conns") == 0 && i + 1 < argc) {
      have_total = parse_u32(argv[++i], &cfg->total_conns) == 0 &&
                   cfg->total_conns > 0 && cfg->total_conns <= RAWUDP_MAX_CONNS;
      continue;
    }
    if (strcmp(argv[i], "--staleness-period-ns") == 0 && i + 1 < argc) {
      if (parse_u64(argv[++i], &cfg->staleness_period_ns) != 0 ||
          cfg->staleness_period_ns == 0) {
        return -1;
      }
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
  if (!have_port) {
    return -1;
  }
  if (!have_scenario) {
    cfg->scenario = SCENARIO_LEGACY;
    return 0;
  }
  if (!have_total) {
    return -1;
  }
  switch (cfg->scenario) {
    case SCENARIO_ENVIRONMENT_BASELINE:
      return validate_traffic(&cfg->input, BK_MIN_PAYLOAD);
    case SCENARIO_AUTHORITATIVE_STATE:
      if (cfg->input.traffic_id == cfg->state.traffic_id ||
          validate_traffic(&cfg->input, BK_MIN_PAYLOAD) != 0) {
        return -1;
      }
      return validate_traffic(&cfg->state,
                              BK_AUTHORITATIVE_STATE_MIN_PAYLOAD);
    case SCENARIO_ROOM_RELAY:
      return validate_traffic(&cfg->publish, BK_MIN_PAYLOAD);
    case SCENARIO_LEGACY:
      break;
  }
  return -1;
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
  int n = 0;
  uint64_t interval_ns = 0;
  if (cfg->state.rate_lt > 0.0) {
    if (interval_from_rate(cfg->state.rate_lt, &interval_ns) != 0) {
      return -1;
    }
    streams[n++] = (bk_stream){
        .must_deliver = false,
        .broadcast = false,
        .traffic_id = cfg->state.traffic_id,
        .direction = BK_DIRECTION_SERVER_TO_CLIENT,
        .interval_ns = interval_ns,
    };
  }
  if (cfg->state.rate_md > 0.0) {
    if (interval_from_rate(cfg->state.rate_md, &interval_ns) != 0) {
      return -1;
    }
    streams[n++] = (bk_stream){
        .must_deliver = true,
        .broadcast = false,
        .traffic_id = cfg->state.traffic_id,
        .direction = BK_DIRECTION_SERVER_TO_CLIENT,
        .interval_ns = interval_ns,
    };
  }
  *n_streams = n;
  return n > 0 ? 0 : -1;
}

static uint64_t addr_key(const struct sockaddr_in *addr) {
  return ((uint64_t)addr->sin_addr.s_addr << 32u) | addr->sin_port;
}

static uint64_t hash_u64(uint64_t x) {
  x ^= x >> 30u;
  x *= 0xbf58476d1ce4e5b9ull;
  x ^= x >> 27u;
  x *= 0x94d049bb133111ebull;
  x ^= x >> 31u;
  return x;
}

static int pt_init(peer_table *pt, size_t origin_cap) {
  memset(pt, 0, sizeof(*pt));
  if (origin_cap == 0) {
    return 0;
  }
  pt->origin_to_peer =
      (size_t *)calloc(origin_cap, sizeof(*pt->origin_to_peer));
  if (pt->origin_to_peer == NULL) {
    return -1;
  }
  pt->origin_cap = origin_cap;
  return 0;
}

static void pt_free(peer_table *pt) {
  free(pt->origin_to_peer);
  free(pt->values);
  free(pt->keys);
  free(pt->peers);
  memset(pt, 0, sizeof(*pt));
}

static void pt_hash_place(peer_table *pt, uint64_t key, size_t value) {
  size_t pos = (size_t)hash_u64(key) & (pt->hcap - 1u);
  while (pt->keys[pos] != 0) {
    pos = (pos + 1u) & (pt->hcap - 1u);
  }
  pt->keys[pos] = key;
  pt->values[pos] = value;
}

static int pt_grow_hash(peer_table *pt) {
  const size_t new_hcap = pt->hcap == 0 ? 1024u : pt->hcap * 2u;
  uint64_t *new_keys = (uint64_t *)calloc(new_hcap, sizeof(*new_keys));
  size_t *new_values = (size_t *)calloc(new_hcap, sizeof(*new_values));
  if (new_keys == NULL || new_values == NULL) {
    free(new_values);
    free(new_keys);
    return -1;
  }
  uint64_t *old_keys = pt->keys;
  size_t *old_values = pt->values;
  const size_t old_hcap = pt->hcap;
  pt->keys = new_keys;
  pt->values = new_values;
  pt->hcap = new_hcap;
  for (size_t i = 0; i < old_hcap; ++i) {
    if (old_keys[i] != 0) {
      pt_hash_place(pt, old_keys[i], old_values[i]);
    }
  }
  free(old_values);
  free(old_keys);
  return 0;
}

// Returns 0 on success, -1 on allocation failure, -2 on identity mismatch.
static int pt_observe(peer_table *pt, const struct sockaddr_in *src,
                      uint32_t origin_id, size_t *peer_index) {
  const uint64_t key = addr_key(src);
  if (pt->hcap != 0) {
    size_t pos = (size_t)hash_u64(key) & (pt->hcap - 1u);
    while (pt->keys[pos] != 0) {
      if (pt->keys[pos] == key) {
        const size_t index = pt->values[pos] - 1u;
        if (pt->peers[index].origin_id != origin_id) {
          return -2;
        }
        *peer_index = index;
        return 0;
      }
      pos = (pos + 1u) & (pt->hcap - 1u);
    }
  }
  if (pt->origin_cap != 0) {
    if (origin_id >= pt->origin_cap || pt->origin_to_peer[origin_id] != 0) {
      return -2;
    }
  }
  if (pt->hcap == 0 || (pt->count + 1u) * 10u >= pt->hcap * 7u) {
    if (pt_grow_hash(pt) != 0) {
      return -1;
    }
  }
  if (pt->count == pt->cap) {
    const size_t new_cap = pt->cap == 0 ? 256u : pt->cap * 2u;
    peer *new_peers =
        (peer *)realloc(pt->peers, new_cap * sizeof(*new_peers));
    if (new_peers == NULL) {
      return -1;
    }
    pt->peers = new_peers;
    pt->cap = new_cap;
  }
  const size_t index = pt->count++;
  pt->peers[index] = (peer){
      .addr = *src,
      .origin_id = origin_id,
      .applied_input_seq = 0,
  };
  if (pt->origin_cap != 0) {
    pt->origin_to_peer[origin_id] = index + 1u;
  }
  pt_hash_place(pt, key, index + 1u);
  *peer_index = index;
  return 0;
}

static bool pt_roster_complete(const peer_table *pt) {
  if (pt->origin_cap == 0 || pt->count != pt->origin_cap) {
    return false;
  }
  for (size_t i = 0; i < pt->origin_cap; ++i) {
    if (pt->origin_to_peer[i] == 0) {
      return false;
    }
  }
  return true;
}

static class_index class_from_flags(uint8_t flags) {
  return (flags & BK_FLAG_MUST_DELIVER) != 0 ? CLASS_MUST_DELIVER
                                             : CLASS_LOSS_TOLERANT;
}

static dist_index dist_from_flags(uint8_t flags) {
  return (flags & BK_FLAG_BROADCAST) != 0 ? DIST_BROADCAST : DIST_ECHO;
}

static bool scenario_client_payload_valid(const server_config *cfg,
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

static void count_submit(server_stats *stats, class_index cls, dist_index dist,
                         bool measured, bool ok) {
  if (!ok) {
    stats->send_failed[cls][dist]++;
    return;
  }
  stats->submit[cls][dist]++;
  if (measured) {
    stats->submit_measured[cls][dist]++;
  }
}

static int set_nonblock(int fd) {
  const int flags = fcntl(fd, F_GETFL, 0);
  return flags < 0 ? -1 : fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static void tune_buffers(int fd) {
  const int buf = RAWUDP_SOCKET_BUFFER_BYTES;
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
}

static int send_to(int fd, const struct sockaddr_in *dst, const void *data,
                   size_t len) {
  for (;;) {
    const ssize_t n = sendto(fd, data, len, 0,
                             (const struct sockaddr *)dst, sizeof(*dst));
    if (n == (ssize_t)len) {
      return 0;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    return -1;
  }
}

static int handle_datagram(int fd, const server_config *cfg, peer_table *pt,
                           const struct sockaddr_in *src, const uint8_t *data,
                           size_t len, server_stats *stats,
                           bk_metrics *metrics) {
  bk_header header;
  if (bk_payload_read(data, len, &header) != 0) {
    stats->invalid_payload++;
    return 0;
  }
  const bool registration = header.seq == 0 && header.sched_ts_ns == 0 &&
                            header.flags == 0 && header.send_ts_ns != 0 &&
                            header.traffic_id == 0 && len == BK_MIN_PAYLOAD;
  if (registration && cfg->scenario != SCENARIO_LEGACY &&
      header.origin_id >= cfg->total_conns) {
    stats->invalid_payload++;
    return 0;
  }
  if (!registration && cfg->scenario != SCENARIO_LEGACY &&
      !scenario_client_payload_valid(cfg, &header, len)) {
    stats->invalid_payload++;
    return 0;
  }
  if (!registration &&
      bk_payload_validate_body(data, len, &header) != 0) {
    stats->invalid_payload++;
    return 0;
  }
  size_t peer_index = 0;
  const int observed = pt_observe(pt, src, header.origin_id, &peer_index);
  if (observed == -1) {
    return -1;
  }
  if (observed != 0) {
    stats->invalid_payload++;
    return 0;
  }

  if (registration) {
    return 0;
  }

  const class_index cls = class_from_flags(header.flags);
  const dist_index dist = dist_from_flags(header.flags);
  const bool measured = (header.flags & BK_FLAG_MEASURE) != 0;
  stats->recv[cls][dist]++;
  if (measured) {
    stats->recv_measured[cls][dist]++;
  }

  if (cfg->scenario == SCENARIO_AUTHORITATIVE_STATE) {
    if ((header.flags & BK_FLAG_MUST_DELIVER) == 0 &&
        header.seq > pt->peers[peer_index].applied_input_seq) {
      pt->peers[peer_index].applied_input_seq = header.seq;
    }
    bk_metrics_on_recv(metrics, header.origin_id, &header, bk_now_ns());
    return 0;
  }

  if (dist == DIST_ECHO) {
    const bool ok = send_to(fd, src, data, len) == 0;
    count_submit(stats, cls, dist, measured, ok);
    return 0;
  }
  for (size_t i = 0; i < pt->count; ++i) {
    const bool ok = send_to(fd, &pt->peers[i].addr, data, len) == 0;
    count_submit(stats, cls, dist, measured, ok);
  }
  return 0;
}

static int service_once(int fd, const server_config *cfg, peer_table *pt,
                        int timeout_ms, server_stats *stats,
                        bk_metrics *metrics) {
  struct pollfd pfd = {fd, POLLIN, 0};
  const int polled = poll(&pfd, 1, timeout_ms);
  if (polled < 0) {
    return errno == EINTR ? 0 : -1;
  }
  if (polled == 0 || (pfd.revents & POLLIN) == 0) {
    return 0;
  }
  static uint8_t buf[RAWUDP_MAX_PAYLOAD_BYTES];
  for (int drained = 0; drained < RAWUDP_SERVER_DRAIN_BUDGET; ++drained) {
    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);
    const ssize_t n = recvfrom(fd, buf, sizeof(buf), MSG_TRUNC,
                               (struct sockaddr *)&src, &src_len);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    }
    if ((size_t)n > sizeof(buf)) {
      stats->invalid_payload++;
      continue;
    }
    if (handle_datagram(fd, cfg, pt, &src, buf, (size_t)n, stats,
                        metrics) != 0) {
      return -1;
    }
  }
  return 0;
}

static size_t state_payload_size(const server_config *cfg,
                                 const bk_slot *slot) {
  return (slot->flags & BK_FLAG_MUST_DELIVER) != 0 ? cfg->state.payload_md
                                                   : cfg->state.payload_lt;
}

static void make_state_header(const server_config *cfg, const bk_slot *slot,
                              uint64_t send_ts_ns, bk_header *header) {
  *header = (bk_header){
      .seq = slot->seq,
      .sched_ts_ns = slot->sched_ts_ns,
      .send_ts_ns = send_ts_ns,
      .flags = (uint8_t)(slot->flags & (uint8_t)~BK_FLAG_BROADCAST),
      .origin_id = cfg->total_conns,
      .traffic_id = slot->traffic_id,
  };
}

static int send_state_slot(int fd, const server_config *cfg,
                           const peer_table *pt, const bk_slot *slot,
                           uint8_t *payload, bk_metrics *metrics,
                           server_stats *stats) {
  if ((slot->flags & (BK_FLAG_MEASURE | BK_FLAG_MUST_DELIVER)) ==
      BK_FLAG_MEASURE) {
    stats->server_state_ticks++;
  }
  const size_t payload_size = state_payload_size(cfg, slot);
  for (uint32_t target = 0; target < cfg->total_conns; ++target) {
    const size_t mapped = pt->origin_to_peer[target];
    bk_header header;
    make_state_header(cfg, slot, 0, &header);
    bool submitted = false;
    if (mapped != 0 &&
        bk_authoritative_state_write_applied_input_seq(
            payload, payload_size,
            pt->peers[mapped - 1u].applied_input_seq) == 0 &&
        bk_authoritative_state_fill_target_pad(payload, payload_size,
                                                target) == 0) {
      header.send_ts_ns = bk_now_ns();
      if (bk_payload_write(payload, payload_size, &header) == 0) {
        submitted = send_to(fd, &pt->peers[mapped - 1u].addr, payload,
                            payload_size) == 0;
      }
    }
    bk_metrics_on_slot(metrics, &header, submitted);
  }
  return 0;
}

static void mark_state_unsent(const server_config *cfg, const peer_table *pt,
                              bk_plan *plan, uint64_t cutoff_ns,
                              bk_metrics *metrics, server_stats *stats) {
  bk_slot slot;
  while (bk_plan_next(plan, cutoff_ns, &slot)) {
    if ((slot.flags & (BK_FLAG_MEASURE | BK_FLAG_MUST_DELIVER)) ==
        BK_FLAG_MEASURE) {
      stats->server_state_ticks++;
    }
    for (uint32_t target = 0; target < cfg->total_conns; ++target) {
      (void)pt;
      bk_header header;
      make_state_header(cfg, &slot, 0, &header);
      bk_metrics_on_slot(metrics, &header, false);
    }
  }
}

static int expect_client_inputs(const server_config *cfg,
                                const bk_schedule *schedule,
                                bk_metrics *metrics) {
  if (cfg->input.rate_lt == 0.0) {
    return 0;
  }
  for (uint32_t origin = 0; origin < cfg->total_conns; ++origin) {
    if (bk_metrics_expect_latest(metrics, origin, origin,
                                 cfg->input.traffic_id,
                                 BK_DIRECTION_CLIENT_TO_SERVER,
                                 schedule->start_at_ns) != 0) {
      return -1;
    }
  }
  return 0;
}

static int open_server_socket(uint16_t port) {
  const int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return -1;
  }
  const int reuse = 1;
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
    close(fd);
    return -1;
  }
  tune_buffers(fd);
  if (set_nonblock(fd) != 0) {
    close(fd);
    return -1;
  }
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(port);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static int format_stats_json(const server_stats *stats, const server_config *cfg,
                             const peer_table *peers, char *buf, size_t cap) {
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
      ",\"broadcast\":%" PRIu64 "}},\"invalid_payload\":%" PRIu64
      ",\"authoritative_progress\":{\"role\":\"server\","
      "\"local_conns\":0,\"roster_conns\":%zu,"
      "\"input_last_sent_min\":0,\"input_last_sent_max\":0,"
      "\"state_header_seq_recv_min\":0,"
      "\"state_header_seq_recv_max\":0,"
      "\"state_applied_input_seq_recv_min\":0,"
      "\"state_applied_input_seq_recv_max\":0,"
      "\"server_state_ticks\":%" PRIu64 "}}",
      stats->recv[CLASS_LOSS_TOLERANT][DIST_ECHO],
      stats->recv[CLASS_LOSS_TOLERANT][DIST_BROADCAST],
      stats->recv[CLASS_MUST_DELIVER][DIST_ECHO],
      stats->recv[CLASS_MUST_DELIVER][DIST_BROADCAST],
      stats->submit[CLASS_LOSS_TOLERANT][DIST_ECHO],
      stats->submit[CLASS_LOSS_TOLERANT][DIST_BROADCAST],
      stats->submit[CLASS_MUST_DELIVER][DIST_ECHO],
      stats->submit[CLASS_MUST_DELIVER][DIST_BROADCAST],
      stats->recv_measured[CLASS_LOSS_TOLERANT][DIST_ECHO],
      stats->recv_measured[CLASS_LOSS_TOLERANT][DIST_BROADCAST],
      stats->recv_measured[CLASS_MUST_DELIVER][DIST_ECHO],
      stats->recv_measured[CLASS_MUST_DELIVER][DIST_BROADCAST],
      stats->submit_measured[CLASS_LOSS_TOLERANT][DIST_ECHO],
      stats->submit_measured[CLASS_LOSS_TOLERANT][DIST_BROADCAST],
      stats->submit_measured[CLASS_MUST_DELIVER][DIST_ECHO],
      stats->submit_measured[CLASS_MUST_DELIVER][DIST_BROADCAST],
      stats->send_failed[CLASS_LOSS_TOLERANT][DIST_ECHO],
      stats->send_failed[CLASS_LOSS_TOLERANT][DIST_BROADCAST],
      stats->send_failed[CLASS_MUST_DELIVER][DIST_ECHO],
      stats->send_failed[CLASS_MUST_DELIVER][DIST_BROADCAST],
      stats->invalid_payload,
      cfg->scenario == SCENARIO_AUTHORITATIVE_STATE ? peers->count : 0u,
      stats->server_state_ticks);
  return n > 0 && (size_t)n < cap ? 0 : -1;
}

int main(int argc, char **argv) {
  server_config cfg;
  if (parse_args(argc, argv, &cfg) != 0) {
    usage(argv[0]);
    return EXIT_FAILURE;
  }
  if (signal(SIGINT, on_signal) == SIG_ERR ||
      signal(SIGTERM, on_signal) == SIG_ERR) {
    perror("signal");
    return EXIT_FAILURE;
  }

  int rc = EXIT_FAILURE;
  const int fd = open_server_socket(cfg.port);
  if (fd < 0) {
    perror("raw_udp server socket");
    return EXIT_FAILURE;
  }
  peer_table peers;
  if (pt_init(&peers, cfg.scenario == SCENARIO_AUTHORITATIVE_STATE
                          ? cfg.total_conns
                          : 0) != 0) {
    close(fd);
    return EXIT_FAILURE;
  }
  server_stats stats;
  memset(&stats, 0, sizeof(stats));

  const uint32_t max_origin_id =
      cfg.scenario == SCENARIO_LEGACY ? 1u : cfg.total_conns + 1u;
  const bk_metrics_config metrics_cfg = {
      .max_origin_id = max_origin_id,
      .deadline_ns = cfg.scenario == SCENARIO_AUTHORITATIVE_STATE
                         ? cfg.input.deadline_ns
                         : 0,
      .staleness_period_ns = cfg.staleness_period_ns,
      .max_local_index = cfg.scenario == SCENARIO_AUTHORITATIVE_STATE
                             ? cfg.total_conns
                             : 0,
  };
  bk_metrics *metrics = bk_metrics_new(&metrics_cfg);
  bk_control *control = NULL;
  if (metrics == NULL) {
    pt_free(&peers);
    close(fd);
    return EXIT_FAILURE;
  }
  if (cfg.scenario == SCENARIO_AUTHORITATIVE_STATE &&
      (bk_metrics_set_traffic_deadline(
           metrics, cfg.input.traffic_id, BK_DIRECTION_CLIENT_TO_SERVER,
           cfg.input.deadline_ns) != 0 ||
       bk_metrics_set_traffic_deadline(
           metrics, cfg.state.traffic_id, BK_DIRECTION_SERVER_TO_CLIENT,
           cfg.state.deadline_ns) != 0)) {
    goto cleanup;
  }

  control = bk_control_connect(NULL);
  bk_schedule schedule = {0, 0, 0};
  bool schedule_valid = false;
  if (control != NULL) {
    if (bk_control_hello(control, "server", "raw_udp", 0) != 0 ||
        bk_control_ready(control, 0) != 0) {
      fprintf(stderr, "benchkit control handshake failed\n");
      goto cleanup;
    }
    while (!g_stop) {
      const int polled = bk_control_poll_schedule(control, &schedule);
      if (polled < 0) {
        fprintf(stderr, "benchkit schedule wait failed\n");
        goto cleanup;
      }
      if (polled == 1) {
        schedule_valid = true;
        break;
      }
      if (service_once(fd, &cfg, &peers, RAWUDP_SERVICE_SLICE_MS, &stats,
                       metrics) != 0) {
        fprintf(stderr, "service failed\n");
        goto cleanup;
      }
    }
    if (!schedule_valid) {
      goto cleanup;
    }
  }

  bk_plan *state_plan = NULL;
  uint8_t *state_payload = NULL;
  bool roster_frozen = false;
  bool state_marked_unsent = false;
  bool window_final = false;
  while (!g_stop) {
    uint64_t now = bk_now_ns();
    if (cfg.scenario == SCENARIO_AUTHORITATIVE_STATE &&
        !schedule_valid && pt_roster_complete(&peers)) {
      schedule.start_at_ns = add_ns(now, DEV_WARMUP_NS);
      schedule.stop_at_ns = add_ns(schedule.start_at_ns, DEV_DURATION_NS);
      schedule.drain_until_ns = add_ns(schedule.stop_at_ns, DEV_DRAIN_NS);
      schedule_valid = true;
    }
    if (cfg.scenario == SCENARIO_AUTHORITATIVE_STATE && schedule_valid &&
        !roster_frozen && now >= schedule.start_at_ns) {
      fprintf(stderr,
              "authoritative roster incomplete before measurement start\n");
      goto loop_cleanup;
    }
    if (cfg.scenario == SCENARIO_AUTHORITATIVE_STATE && schedule_valid &&
        !roster_frozen && pt_roster_complete(&peers)) {
      bk_stream streams[2];
      int n_streams = 0;
      if (build_state_streams(&cfg, streams, &n_streams) != 0) {
        fprintf(stderr, "invalid state rates\n");
        goto loop_cleanup;
      }
      const size_t state_cap = cfg.state.payload_lt > cfg.state.payload_md
                                   ? cfg.state.payload_lt
                                   : cfg.state.payload_md;
      state_payload = (uint8_t *)calloc(1, state_cap);
      state_plan = bk_plan_new(streams, n_streams, now,
                               schedule.start_at_ns, schedule.stop_at_ns);
      if (state_payload == NULL || state_plan == NULL ||
          expect_client_inputs(&cfg, &schedule, metrics) != 0) {
        fprintf(stderr, "authoritative state initialization failed\n");
        goto loop_cleanup;
      }
      roster_frozen = true;
    }

    if (control != NULL && schedule_valid && !window_final) {
      if (now >= schedule.start_at_ns) {
        window_final = true;
      } else {
        const int polled = bk_control_poll_window(control, &schedule);
        if (polled < 0) {
          fprintf(stderr, "benchkit window poll failed\n");
          goto loop_cleanup;
        }
        if (polled == 1) {
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
        if (send_state_slot(fd, &cfg, &peers, &slot, state_payload,
                            metrics, &stats) != 0) {
          goto loop_cleanup;
        }
      }
    } else if (state_plan != NULL && !state_marked_unsent) {
      const uint64_t cutoff =
          schedule.stop_at_ns == 0 ? 0 : schedule.stop_at_ns - 1u;
      mark_state_unsent(&cfg, &peers, state_plan, cutoff, metrics, &stats);
      state_marked_unsent = true;
    }
    if (control != NULL && schedule_valid && now >= schedule.drain_until_ns) {
      break;
    }

    int timeout_ms = RAWUDP_SERVICE_SLICE_MS;
    if (state_plan != NULL && now < schedule.stop_at_ns) {
      const uint64_t due = bk_plan_peek_ns(state_plan);
      if (due <= now) {
        timeout_ms = 0;
      } else {
        const uint64_t wait_ms = (due - now) / 1000000ull;
        if (wait_ms < (uint64_t)timeout_ms) {
          timeout_ms = (int)wait_ms;
        }
      }
    }
    if (service_once(fd, &cfg, &peers, timeout_ms, &stats, metrics) != 0) {
      fprintf(stderr, "service failed\n");
      goto loop_cleanup;
    }
  }

  if (state_plan != NULL && !state_marked_unsent) {
    const uint64_t cutoff =
        schedule.stop_at_ns == 0 ? 0 : schedule.stop_at_ns - 1u;
    mark_state_unsent(&cfg, &peers, state_plan, cutoff, metrics, &stats);
  }

  {
    const char *metrics_path = getenv("BENCH_METRICS_OUT");
    if (metrics_path != NULL && *metrics_path != '\0' &&
        bk_metrics_dump_json(metrics, metrics_path) != 0) {
      fprintf(stderr, "bk_metrics_dump_json failed\n");
      goto loop_cleanup;
    }
    char stats_json[2048];
    if (format_stats_json(&stats, &cfg, &peers, stats_json,
                          sizeof(stats_json)) != 0) {
      goto loop_cleanup;
    }
    if (control != NULL) {
      if (bk_control_done(control, stats_json) != 0) {
        fprintf(stderr, "benchkit done failed\n");
        goto loop_cleanup;
      }
    }
  }
  rc = EXIT_SUCCESS;

loop_cleanup:
  free(state_payload);
  bk_plan_free(state_plan);

cleanup:
  if (control != NULL) {
    bk_control_close(control);
  }
  bk_metrics_free(metrics);
  pt_free(&peers);
  close(fd);
  return rc;
}
