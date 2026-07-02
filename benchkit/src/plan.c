#include "benchkit.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct bk_plan {
  bk_stream *streams;
  uint64_t *next_sched_ns;
  uint64_t *next_seq;
  int n_streams;
  uint64_t measure_start_ns;
  uint64_t measure_stop_ns;
};

static uint64_t add_interval(uint64_t a, uint64_t b) {
  if (UINT64_MAX - a < b) {
    return UINT64_MAX;
  }
  return a + b;
}

bk_plan *bk_plan_new(const bk_stream *streams, int n_streams, uint64_t start_ns,
                     uint64_t measure_start_ns, uint64_t measure_stop_ns) {
  if (n_streams < 0 || (n_streams > 0 && streams == NULL)) {
    return NULL;
  }
  for (int i = 0; i < n_streams; ++i) {
    if (streams[i].interval_ns == 0) {
      return NULL;
    }
  }

  bk_plan *p = (bk_plan *)calloc(1, sizeof(*p));
  if (p == NULL) {
    return NULL;
  }
  p->n_streams = n_streams;
  p->measure_start_ns = measure_start_ns;
  p->measure_stop_ns = measure_stop_ns;

  const size_t n = (size_t)n_streams;
  if (n > 0) {
    p->streams = (bk_stream *)calloc(n, sizeof(*p->streams));
    p->next_sched_ns = (uint64_t *)calloc(n, sizeof(*p->next_sched_ns));
    p->next_seq = (uint64_t *)calloc(n, sizeof(*p->next_seq));
    if (p->streams == NULL || p->next_sched_ns == NULL || p->next_seq == NULL) {
      bk_plan_free(p);
      return NULL;
    }
    memcpy(p->streams, streams, n * sizeof(*p->streams));
    for (int i = 0; i < n_streams; ++i) {
      p->next_sched_ns[i] = start_ns;
      p->next_seq[i] = 1;
    }
  }
  return p;
}

void bk_plan_free(bk_plan *p) {
  if (p == NULL) {
    return;
  }
  free(p->streams);
  free(p->next_sched_ns);
  free(p->next_seq);
  free(p);
}

uint64_t bk_plan_peek_ns(const bk_plan *p) {
  if (p == NULL || p->n_streams == 0) {
    return UINT64_MAX;
  }

  uint64_t best = UINT64_MAX;
  for (int i = 0; i < p->n_streams; ++i) {
    if (p->next_sched_ns[i] < best) {
      best = p->next_sched_ns[i];
    }
  }
  return best;
}

bool bk_plan_next(bk_plan *p, uint64_t now_ns, bk_slot *out) {
  if (p == NULL || out == NULL || p->n_streams == 0) {
    return false;
  }

  int best_index = -1;
  uint64_t best_sched = UINT64_MAX;
  for (int i = 0; i < p->n_streams; ++i) {
    if (p->next_sched_ns[i] < best_sched) {
      best_sched = p->next_sched_ns[i];
      best_index = i;
    }
  }
  if (best_index < 0 || best_sched > now_ns) {
    return false;
  }

  const bk_stream *s = &p->streams[best_index];
  uint8_t flags = 0;
  if (s->must_deliver) {
    flags |= BK_FLAG_MUST_DELIVER;
  }
  if (s->broadcast) {
    flags |= BK_FLAG_BROADCAST;
  }
  if (best_sched >= p->measure_start_ns && best_sched < p->measure_stop_ns) {
    flags |= BK_FLAG_MEASURE;
  }

  out->sched_ts_ns = best_sched;
  out->seq = p->next_seq[best_index];
  out->stream_index = best_index;
  out->flags = flags;

  p->next_sched_ns[best_index] =
      add_interval(p->next_sched_ns[best_index], s->interval_ns);
  if (p->next_seq[best_index] != UINT64_MAX) {
    p->next_seq[best_index]++;
  }
  return true;
}
