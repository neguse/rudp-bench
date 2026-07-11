#ifndef RUDP_BENCH_SCENARIO_CLI_H
#define RUDP_BENCH_SCENARIO_CLI_H

#include "benchkit.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
  BK_SCENARIO_LEGACY = 0,
  BK_SCENARIO_ENVIRONMENT_BASELINE,
  BK_SCENARIO_AUTHORITATIVE_STATE,
  BK_SCENARIO_ROOM_RELAY,
} bk_scenario_kind;

typedef struct {
  uint8_t traffic_id;
  double rate_lt;
  double rate_md;
  size_t payload_lt;
  size_t payload_md;
  uint64_t deadline_ns;
  unsigned present;
} bk_scenario_traffic;

typedef struct {
  bk_scenario_kind kind;
  uint32_t total_conns;
  int present;
  int have_total_conns;
  bk_scenario_traffic input;
  bk_scenario_traffic state;
  bk_scenario_traffic publish;
} bk_scenario_cli;

typedef struct {
  uint32_t local_conns;
  uint32_t roster_conns;
  uint64_t input_last_sent_min;
  uint64_t input_last_sent_max;
  uint64_t state_header_seq_recv_min;
  uint64_t state_header_seq_recv_max;
  uint64_t state_applied_input_seq_recv_min;
  uint64_t state_applied_input_seq_recv_max;
  uint64_t server_state_ticks;
} bk_authoritative_progress;

// Writes a complete `"authoritative_progress": {...}` member. Callers append
// it to their transport-specific done stats object, keeping one stable schema
// across native adapters.
static inline int bk_authoritative_progress_format(
    const char *role, const bk_authoritative_progress *progress, char *buf,
    size_t cap) {
  const int n = snprintf(
      buf, cap,
      "\"authoritative_progress\":{\"role\":\"%s\","
      "\"local_conns\":%u,\"roster_conns\":%u,"
      "\"input_last_sent_min\":%" PRIu64
      ",\"input_last_sent_max\":%" PRIu64
      ",\"state_header_seq_recv_min\":%" PRIu64
      ",\"state_header_seq_recv_max\":%" PRIu64
      ",\"state_applied_input_seq_recv_min\":%" PRIu64
      ",\"state_applied_input_seq_recv_max\":%" PRIu64
      ",\"server_state_ticks\":%" PRIu64 "}",
      role, progress->local_conns, progress->roster_conns,
      progress->input_last_sent_min, progress->input_last_sent_max,
      progress->state_header_seq_recv_min,
      progress->state_header_seq_recv_max,
      progress->state_applied_input_seq_recv_min,
      progress->state_applied_input_seq_recv_max,
      progress->server_state_ticks);
  return n > 0 && (size_t)n < cap ? 0 : -1;
}

#define BK_SCENARIO_HAVE_ID (1u << 0)
#define BK_SCENARIO_HAVE_RATE_LT (1u << 1)
#define BK_SCENARIO_HAVE_RATE_MD (1u << 2)
#define BK_SCENARIO_HAVE_PAYLOAD_LT (1u << 3)
#define BK_SCENARIO_HAVE_PAYLOAD_MD (1u << 4)
#define BK_SCENARIO_HAVE_DEADLINE (1u << 5)
#define BK_SCENARIO_HAVE_ALL ((1u << 6) - 1u)

static inline int bk_scenario_parse_u64(const char *text, uint64_t *out) {
  if (text == NULL || *text == '\0') {
    return -1;
  }
  errno = 0;
  char *end = NULL;
  const unsigned long long value = strtoull(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0') {
    return -1;
  }
  *out = (uint64_t)value;
  return 0;
}

static inline int bk_scenario_parse_rate(const char *text, double *out) {
  if (text == NULL || *text == '\0') {
    return -1;
  }
  errno = 0;
  char *end = NULL;
  const double value = strtod(text, &end);
  if (errno != 0 || end == text || *end != '\0' || !(value >= 0.0)) {
    return -1;
  }
  *out = value;
  return 0;
}

static inline int bk_scenario_parse_traffic(const char *arg,
                                            const char *value,
                                            const char *prefix,
                                            bk_scenario_traffic *traffic) {
  const size_t prefix_len = strlen(prefix);
  if (strncmp(arg, "--", 2) != 0 ||
      strncmp(arg + 2, prefix, prefix_len) != 0 ||
      arg[2 + prefix_len] != '-') {
    return 0;
  }
  const char *field = arg + 3 + prefix_len;
  if (strcmp(field, "traffic-id") == 0) {
    uint64_t parsed = 0;
    if (bk_scenario_parse_u64(value, &parsed) != 0 || parsed == 0 ||
        parsed > UINT8_MAX) {
      return -1;
    }
    traffic->traffic_id = (uint8_t)parsed;
    traffic->present |= BK_SCENARIO_HAVE_ID;
  } else if (strcmp(field, "rate-lt") == 0) {
    if (bk_scenario_parse_rate(value, &traffic->rate_lt) != 0) {
      return -1;
    }
    traffic->present |= BK_SCENARIO_HAVE_RATE_LT;
  } else if (strcmp(field, "rate-md") == 0) {
    if (bk_scenario_parse_rate(value, &traffic->rate_md) != 0) {
      return -1;
    }
    traffic->present |= BK_SCENARIO_HAVE_RATE_MD;
  } else if (strcmp(field, "payload-lt") == 0) {
    uint64_t parsed = 0;
    if (bk_scenario_parse_u64(value, &parsed) != 0 || parsed > SIZE_MAX) {
      return -1;
    }
    traffic->payload_lt = (size_t)parsed;
    traffic->present |= BK_SCENARIO_HAVE_PAYLOAD_LT;
  } else if (strcmp(field, "payload-md") == 0) {
    uint64_t parsed = 0;
    if (bk_scenario_parse_u64(value, &parsed) != 0 || parsed > SIZE_MAX) {
      return -1;
    }
    traffic->payload_md = (size_t)parsed;
    traffic->present |= BK_SCENARIO_HAVE_PAYLOAD_MD;
  } else if (strcmp(field, "deadline-ns") == 0) {
    if (bk_scenario_parse_u64(value, &traffic->deadline_ns) != 0) {
      return -1;
    }
    traffic->present |= BK_SCENARIO_HAVE_DEADLINE;
  } else {
    return 0;
  }
  return 1;
}

// Returns 1 when argv[*index] was consumed, 0 when it is not a scenario arg,
// and -1 on malformed input. Value-taking options advance *index.
static inline int bk_scenario_cli_parse(int argc, char **argv, int *index,
                                        bk_scenario_cli *scenario) {
  const char *arg = argv[*index];
  if (strcmp(arg, "--scenario") == 0) {
    if (*index + 1 >= argc) {
      return -1;
    }
    const char *value = argv[++*index];
    if (strcmp(value, "environment_baseline") == 0) {
      scenario->kind = BK_SCENARIO_ENVIRONMENT_BASELINE;
    } else if (strcmp(value, "authoritative_state") == 0) {
      scenario->kind = BK_SCENARIO_AUTHORITATIVE_STATE;
    } else if (strcmp(value, "room_relay") == 0) {
      scenario->kind = BK_SCENARIO_ROOM_RELAY;
    } else {
      return -1;
    }
    scenario->present = 1;
    return 1;
  }
  if (strcmp(arg, "--total-conns") == 0) {
    uint64_t parsed = 0;
    if (*index + 1 >= argc ||
        bk_scenario_parse_u64(argv[++*index], &parsed) != 0 || parsed == 0 ||
        parsed > UINT32_MAX) {
      return -1;
    }
    scenario->total_conns = (uint32_t)parsed;
    scenario->have_total_conns = 1;
    return 1;
  }
  if (*index + 1 >= argc) {
    return 0;
  }
  int parsed = bk_scenario_parse_traffic(arg, argv[*index + 1], "input",
                                         &scenario->input);
  if (parsed == 0) {
    parsed = bk_scenario_parse_traffic(arg, argv[*index + 1], "state",
                                       &scenario->state);
  }
  if (parsed == 0) {
    parsed = bk_scenario_parse_traffic(arg, argv[*index + 1], "publish",
                                       &scenario->publish);
  }
  if (parsed == 1) {
    ++*index;
  }
  return parsed;
}

static inline int bk_scenario_traffic_valid(
    const bk_scenario_traffic *traffic, size_t min_payload,
    size_t max_payload) {
  if (traffic->present != BK_SCENARIO_HAVE_ALL ||
      (traffic->rate_lt == 0.0 && traffic->rate_md == 0.0)) {
    return 0;
  }
  if (traffic->rate_lt > 0.0 &&
      (traffic->payload_lt < min_payload ||
       traffic->payload_lt > max_payload)) {
    return 0;
  }
  if (traffic->rate_md > 0.0 &&
      (traffic->payload_md < min_payload ||
       traffic->payload_md > max_payload)) {
    return 0;
  }
  return 1;
}

static inline int bk_scenario_cli_validate(const bk_scenario_cli *scenario,
                                           uint32_t max_conns,
                                           size_t max_payload) {
  if (!scenario->present) {
    return 0;
  }
  if (!scenario->have_total_conns || scenario->total_conns > max_conns) {
    return -1;
  }
  switch (scenario->kind) {
    case BK_SCENARIO_ENVIRONMENT_BASELINE:
      return bk_scenario_traffic_valid(&scenario->input, BK_MIN_PAYLOAD,
                                       max_payload)
                 ? 0
                 : -1;
    case BK_SCENARIO_AUTHORITATIVE_STATE:
      return scenario->input.traffic_id != scenario->state.traffic_id &&
                     bk_scenario_traffic_valid(&scenario->input,
                                               BK_MIN_PAYLOAD, max_payload) &&
                     bk_scenario_traffic_valid(
                         &scenario->state,
                         BK_AUTHORITATIVE_STATE_MIN_PAYLOAD, max_payload)
                 ? 0
                 : -1;
    case BK_SCENARIO_ROOM_RELAY:
      return bk_scenario_traffic_valid(&scenario->publish, BK_MIN_PAYLOAD,
                                       max_payload)
                 ? 0
                 : -1;
    case BK_SCENARIO_LEGACY:
      break;
  }
  return -1;
}

static inline const bk_scenario_traffic *bk_scenario_client_traffic(
    const bk_scenario_cli *scenario, bk_direction *direction,
    int *broadcast) {
  *broadcast = 0;
  switch (scenario->kind) {
    case BK_SCENARIO_ENVIRONMENT_BASELINE:
      *direction = BK_DIRECTION_ROOM_RELAY;
      return &scenario->input;
    case BK_SCENARIO_AUTHORITATIVE_STATE:
      *direction = BK_DIRECTION_CLIENT_TO_SERVER;
      return &scenario->input;
    case BK_SCENARIO_ROOM_RELAY:
      *direction = BK_DIRECTION_ROOM_RELAY;
      *broadcast = 1;
      return &scenario->publish;
    case BK_SCENARIO_LEGACY:
      break;
  }
  return NULL;
}

static inline int bk_payload_is_registration(const bk_header *header,
                                             size_t payload_size) {
  return header != NULL && payload_size == BK_MIN_PAYLOAD &&
         header->seq == 0 && header->sched_ts_ns == 0 &&
         header->send_ts_ns != 0 && header->flags == 0 &&
         header->traffic_id == BK_TRAFFIC_ID_ROOM_RELAY;
}

// Validates the immutable wire shape of a client-origin scenario message.
// Transport-specific class routing and body integrity are checked by callers.
static inline int bk_scenario_client_payload_valid(
    const bk_scenario_cli *scenario, const bk_header *header,
    size_t payload_size) {
  if (scenario == NULL || header == NULL || !scenario->present ||
      header->origin_id >= scenario->total_conns || header->seq == 0 ||
      header->sched_ts_ns == 0 || header->send_ts_ns == 0) {
    return 0;
  }
  bk_direction direction = BK_DIRECTION_ROOM_RELAY;
  int broadcast = 0;
  const bk_scenario_traffic *traffic =
      bk_scenario_client_traffic(scenario, &direction, &broadcast);
  if (traffic == NULL || header->traffic_id != traffic->traffic_id ||
      BK_FLAGS_DIRECTION(header->flags) != direction ||
      (((header->flags & BK_FLAG_BROADCAST) != 0) != (broadcast != 0))) {
    return 0;
  }
  const int must_deliver = (header->flags & BK_FLAG_MUST_DELIVER) != 0;
  const double rate = must_deliver ? traffic->rate_md : traffic->rate_lt;
  const size_t expected_size =
      must_deliver ? traffic->payload_md : traffic->payload_lt;
  return rate > 0.0 && payload_size == expected_size;
}

static inline int bk_scenario_state_payload_valid(
    const bk_scenario_cli *scenario, const bk_header *header,
    size_t payload_size) {
  if (scenario == NULL || header == NULL ||
      scenario->kind != BK_SCENARIO_AUTHORITATIVE_STATE ||
      header->origin_id != scenario->total_conns || header->seq == 0 ||
      header->sched_ts_ns == 0 || header->send_ts_ns == 0 ||
      header->traffic_id != scenario->state.traffic_id ||
      BK_FLAGS_DIRECTION(header->flags) != BK_DIRECTION_SERVER_TO_CLIENT ||
      (header->flags & BK_FLAG_BROADCAST) != 0) {
    return 0;
  }
  const int must_deliver = (header->flags & BK_FLAG_MUST_DELIVER) != 0;
  const double rate =
      must_deliver ? scenario->state.rate_md : scenario->state.rate_lt;
  const size_t expected_size = must_deliver ? scenario->state.payload_md
                                            : scenario->state.payload_lt;
  return rate > 0.0 && payload_size == expected_size;
}

#endif
