#ifndef RUDP_BENCH_SERVERS_RAMP_H
#define RUDP_BENCH_SERVERS_RAMP_H

#include "benchkit.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
  int enabled;
  uint32_t start_conns;
  uint32_t step_conns;
  uint64_t guard_ns;
  uint64_t sample_ns;
  uint64_t drain_ns;
} bk_ramp_config;

typedef struct {
  uint32_t phase;
  uint32_t target_conns;
  uint64_t phase_start_ns;
  uint64_t reset_at_ns;
  uint64_t sample_end_ns;
  uint64_t phase_end_ns;
  uint32_t sample_conns;
  int reset_done;  // metrics reset/cohort/expected flows prepared at phase start
} bk_ramp_phase;

static inline int bk_ramp_parse_u64(const char *text, uint64_t *out) {
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

static inline uint64_t bk_ramp_add_ns(uint64_t a, uint64_t b) {
  return UINT64_MAX - a < b ? UINT64_MAX : a + b;
}

// Ramp mode is deliberately all-or-nothing so a stray environment variable
// cannot silently change the canonical fixed-connection benchmark.
static inline int bk_ramp_config_load(uint32_t max_conns,
                                      bk_ramp_config *out) {
  if (out == NULL) {
    return -1;
  }
  memset(out, 0, sizeof(*out));
  const char *start = getenv("BENCH_RAMP_START_CONNS");
  const char *step = getenv("BENCH_RAMP_STEP_CONNS");
  const char *guard = getenv("BENCH_RAMP_GUARD_NS");
  const char *sample = getenv("BENCH_RAMP_SAMPLE_NS");
  const char *drain = getenv("BENCH_RAMP_DRAIN_NS");
  const int present = (start != NULL) + (step != NULL) + (guard != NULL) +
                      (sample != NULL) + (drain != NULL);
  if (present == 0) {
    return 0;
  }
  if (present != 5) {
    return -1;
  }

  uint64_t parsed_start = 0;
  uint64_t parsed_step = 0;
  if (bk_ramp_parse_u64(start, &parsed_start) != 0 || parsed_start == 0 ||
      parsed_start > max_conns || parsed_start > UINT32_MAX ||
      bk_ramp_parse_u64(step, &parsed_step) != 0 || parsed_step == 0 ||
      parsed_step > UINT32_MAX ||
      bk_ramp_parse_u64(guard, &out->guard_ns) != 0 ||
      bk_ramp_parse_u64(sample, &out->sample_ns) != 0 ||
      bk_ramp_parse_u64(drain, &out->drain_ns) != 0 ||
      out->sample_ns == 0) {
    return -1;
  }
  out->enabled = 1;
  out->start_conns = (uint32_t)parsed_start;
  out->step_conns = (uint32_t)parsed_step;
  return 0;
}

static inline void bk_ramp_phase_begin(const bk_ramp_config *cfg,
                                       uint64_t start_ns,
                                       bk_ramp_phase *phase) {
  memset(phase, 0, sizeof(*phase));
  phase->target_conns = cfg->start_conns;
  phase->phase_start_ns = start_ns;
  phase->reset_at_ns = bk_ramp_add_ns(start_ns, cfg->guard_ns);
  phase->sample_end_ns =
      bk_ramp_add_ns(phase->reset_at_ns, cfg->sample_ns);
  phase->phase_end_ns =
      bk_ramp_add_ns(phase->sample_end_ns, cfg->drain_ns);
}

// Advances to the next absolute phase. Returns 1 when another target exists,
// or 0 after the max-connection phase has completed.
static inline int bk_ramp_phase_advance(const bk_ramp_config *cfg,
                                        uint32_t max_conns,
                                        bk_ramp_phase *phase) {
  if (phase->target_conns >= max_conns) {
    return 0;
  }
  const uint32_t remaining = max_conns - phase->target_conns;
  phase->target_conns +=
      cfg->step_conns < remaining ? cfg->step_conns : remaining;
  phase->phase++;
  phase->phase_start_ns = phase->phase_end_ns;
  phase->reset_at_ns =
      bk_ramp_add_ns(phase->phase_start_ns, cfg->guard_ns);
  phase->sample_end_ns =
      bk_ramp_add_ns(phase->reset_at_ns, cfg->sample_ns);
  phase->phase_end_ns =
      bk_ramp_add_ns(phase->sample_end_ns, cfg->drain_ns);
  phase->sample_conns = 0;
  phase->reset_done = 0;
  return 1;
}

static inline int bk_ramp_dump_metrics(const bk_metrics *metrics,
                                       uint32_t phase,
                                       uint32_t active_conns) {
  const char *base = getenv("BENCH_METRICS_OUT");
  if (base == NULL || *base == '\0') {
    return -1;
  }
  const size_t cap = strlen(base) + 64u;
  char *path = (char *)malloc(cap);
  if (path == NULL) {
    return -1;
  }
  const int n = snprintf(path, cap, "%s.ramp-%06" PRIu32 "-c%06" PRIu32
                                    ".json",
                         base, phase, active_conns);
  const int rc = n > 0 && (size_t)n < cap
                     ? bk_metrics_dump_json(metrics, path)
                     : -1;
  free(path);
  return rc;
}

static inline int bk_ramp_stop_requested(void) {
  const char *path = getenv("BENCH_RAMP_STOP_PATH");
  return path != NULL && *path != '\0' && access(path, F_OK) == 0;
}

#endif  // RUDP_BENCH_SERVERS_RAMP_H
