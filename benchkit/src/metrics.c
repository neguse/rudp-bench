#include "benchkit_internal.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BK_DEFAULT_STALENESS_PERIOD_NS 10000000ull
#define BK_HIST_MIN_NS 1000ull
#define BK_HIST_MAX_NS 100000000000ull
#define BK_HIST_SUBBINS 16u
#define BK_HIST_MAJOR_BINS 28u
#define BK_HIST_BINS (BK_HIST_SUBBINS * BK_HIST_MAJOR_BINS)

typedef struct {
  uint64_t count;
  uint64_t bins[BK_HIST_BINS];
} bk_hist;

typedef struct {
  bool has;
  uint64_t sched_ts_ns;
} bk_latest;

typedef struct {
  uint64_t seq;
  uint32_t origin_id;
  uint32_t local_index;  // 受信側 local conn。broadcast の複製と重複を区別する
  uint8_t class_index;
  bool used;
} bk_seen_key;

typedef struct {
  bk_class_counts counts;
  bk_hist latency_sched;
  bk_hist latency_send;
} bk_class_metrics;

struct bk_metrics {
  bk_metrics_config cfg;
  bk_class_metrics classes[BK_N_CLASSES];
  bk_hist staleness;
  bk_latest *latest;
  uint64_t next_staleness_sample_ns;
  bk_seen_key *seen;
  size_t seen_cap;
  size_t seen_len;
  uint64_t raw_slots;
  uint64_t raw_submitted;
  uint64_t raw_recv_measured;
  uint64_t raw_recv_unmeasured;
};

static uint64_t hash_mix_u64(uint64_t x) {
  x ^= x >> 33u;
  x *= 0xff51afd7ed558ccdull;
  x ^= x >> 33u;
  x *= 0xc4ceb9fe1a85ec53ull;
  x ^= x >> 33u;
  return x;
}

static uint64_t seen_hash(uint32_t local_index, uint32_t origin_id,
                          uint8_t class_index, uint64_t seq) {
  uint64_t x = seq;
  x ^= (uint64_t)origin_id * 0x9e3779b185ebca87ull;
  x ^= (uint64_t)local_index * 0xc2b2ae3d27d4eb4full;
  x ^= (uint64_t)class_index << 56u;
  return hash_mix_u64(x);
}

static bool key_equal(const bk_seen_key *k, uint32_t local_index,
                      uint32_t origin_id, uint8_t class_index, uint64_t seq) {
  return k->used && k->local_index == local_index &&
         k->origin_id == origin_id && k->class_index == class_index &&
         k->seq == seq;
}

static int seen_rehash(bk_metrics *m, size_t new_cap) {
  bk_seen_key *old = m->seen;
  const size_t old_cap = m->seen_cap;
  bk_seen_key *next = (bk_seen_key *)calloc(new_cap, sizeof(*next));
  if (next == NULL) {
    return -1;
  }

  m->seen = next;
  m->seen_cap = new_cap;
  m->seen_len = 0;

  for (size_t i = 0; i < old_cap; ++i) {
    if (!old[i].used) {
      continue;
    }
    const uint64_t h = seen_hash(old[i].local_index, old[i].origin_id,
                                 old[i].class_index, old[i].seq);
    size_t pos = (size_t)h & (m->seen_cap - 1u);
    while (m->seen[pos].used) {
      pos = (pos + 1u) & (m->seen_cap - 1u);
    }
    m->seen[pos] = old[i];
    m->seen_len++;
  }
  free(old);
  return 0;
}

static int seen_insert_new(bk_metrics *m, uint32_t local_index,
                           uint32_t origin_id, uint8_t class_index,
                           uint64_t seq, bool *is_new) {
  if ((m->seen_len + 1u) * 10u >= m->seen_cap * 7u) {
    if (seen_rehash(m, m->seen_cap * 2u) != 0) {
      return -1;
    }
  }

  const uint64_t h = seen_hash(local_index, origin_id, class_index, seq);
  size_t pos = (size_t)h & (m->seen_cap - 1u);
  while (m->seen[pos].used) {
    if (key_equal(&m->seen[pos], local_index, origin_id, class_index, seq)) {
      *is_new = false;
      return 0;
    }
    pos = (pos + 1u) & (m->seen_cap - 1u);
  }

  m->seen[pos].used = true;
  m->seen[pos].local_index = local_index;
  m->seen[pos].origin_id = origin_id;
  m->seen[pos].class_index = class_index;
  m->seen[pos].seq = seq;
  m->seen_len++;
  *is_new = true;
  return 0;
}

static unsigned hist_index(uint64_t value_ns) {
  if (value_ns <= BK_HIST_MIN_NS) {
    return 0;
  }
  if (value_ns >= BK_HIST_MAX_NS) {
    return BK_HIST_BINS - 1u;
  }

  uint64_t low = BK_HIST_MIN_NS;
  unsigned major = 0;
  while (major + 1u < BK_HIST_MAJOR_BINS && value_ns >= low * 2ull) {
    low *= 2ull;
    major++;
  }

  uint64_t offset = value_ns - low;
  unsigned sub = (unsigned)((offset * BK_HIST_SUBBINS) / low);
  if (sub >= BK_HIST_SUBBINS) {
    sub = BK_HIST_SUBBINS - 1u;
  }
  return major * BK_HIST_SUBBINS + sub;
}

static uint64_t hist_bin_upper_ns(unsigned index) {
  if (index == 0) {
    return BK_HIST_MIN_NS;
  }
  if (index >= BK_HIST_BINS - 1u) {
    return BK_HIST_MAX_NS;
  }
  const unsigned major = index / BK_HIST_SUBBINS;
  const unsigned sub = index % BK_HIST_SUBBINS;
  const uint64_t low = BK_HIST_MIN_NS << major;
  uint64_t upper =
      low + ((low * (uint64_t)(sub + 1u) + BK_HIST_SUBBINS - 1u) /
             BK_HIST_SUBBINS);
  if (upper > BK_HIST_MAX_NS) {
    upper = BK_HIST_MAX_NS;
  }
  return upper;
}

static void hist_add(bk_hist *h, uint64_t value_ns) {
  const unsigned index = hist_index(value_ns);
  h->bins[index]++;
  h->count++;
}

static uint64_t hist_percentile(const bk_hist *h, double p) {
  if (h == NULL || h->count == 0) {
    return 0;
  }
  if (p <= 0.0) {
    p = 0.0;
  } else if (p > 1.0) {
    p = 1.0;
  }

  uint64_t rank = (uint64_t)((double)h->count * p);
  if ((double)rank < (double)h->count * p) {
    rank++;
  }
  if (rank == 0) {
    rank = 1;
  }

  uint64_t cumulative = 0;
  for (unsigned i = 0; i < BK_HIST_BINS; ++i) {
    cumulative += h->bins[i];
    if (cumulative >= rank) {
      return hist_bin_upper_ns(i);
    }
  }
  return BK_HIST_MAX_NS;
}

bk_metrics *bk_metrics_new(const bk_metrics_config *cfg) {
  bk_metrics_config local = {0};
  if (cfg != NULL) {
    local = *cfg;
  }
  if (local.max_origin_id == 0) {
    local.max_origin_id = 1;
  }
  if (local.staleness_period_ns == 0) {
    local.staleness_period_ns = BK_DEFAULT_STALENESS_PERIOD_NS;
  }

  bk_metrics *m = (bk_metrics *)calloc(1, sizeof(*m));
  if (m == NULL) {
    return NULL;
  }
  m->cfg = local;

  const size_t latest_count = (size_t)local.max_origin_id * BK_N_CLASSES;
  if (latest_count / BK_N_CLASSES != (size_t)local.max_origin_id) {
    free(m);
    return NULL;
  }
  m->latest = (bk_latest *)calloc(latest_count, sizeof(*m->latest));
  m->seen_cap = 1024u;
  m->seen = (bk_seen_key *)calloc(m->seen_cap, sizeof(*m->seen));
  if (m->latest == NULL || m->seen == NULL) {
    bk_metrics_free(m);
    return NULL;
  }
  return m;
}

void bk_metrics_free(bk_metrics *m) {
  if (m == NULL) {
    return;
  }
  free(m->latest);
  free(m->seen);
  free(m);
}

void bk_metrics_on_slot(bk_metrics *m, const bk_header *h, bool submitted) {
  if (m == NULL || h == NULL) {
    return;
  }
  m->raw_slots++;
  if (submitted) {
    m->raw_submitted++;
  }
  if ((h->flags & BK_FLAG_MEASURE) == 0) {
    return;
  }

  const int class_index = bk_class_index_from_flags(h->flags);
  m->classes[class_index].counts.slots++;
  if ((h->flags & BK_FLAG_BROADCAST) != 0) {
    m->classes[class_index].counts.slots_broadcast++;
  }
  if (submitted) {
    m->classes[class_index].counts.submitted++;
  }
}

void bk_metrics_on_recv(bk_metrics *m, uint32_t local_index,
                        const bk_header *h, uint64_t recv_ts_ns) {
  if (m == NULL || h == NULL) {
    return;
  }
  if ((h->flags & BK_FLAG_MEASURE) == 0) {
    m->raw_recv_unmeasured++;
    return;
  }

  m->raw_recv_measured++;
  const uint8_t class_index = (uint8_t)bk_class_index_from_flags(h->flags);
  bool is_new = false;
  if (seen_insert_new(m, local_index, h->origin_id, class_index, h->seq,
                      &is_new) != 0) {
    return;
  }
  if (!is_new) {
    m->classes[class_index].counts.duplicates++;
    return;
  }

  bk_class_metrics *cm = &m->classes[class_index];
  cm->counts.delivered_unique++;
  const uint64_t sched_latency =
      bk_saturating_sub_u64(recv_ts_ns, h->sched_ts_ns);
  const uint64_t send_latency =
      bk_saturating_sub_u64(recv_ts_ns, h->send_ts_ns);
  hist_add(&cm->latency_sched, sched_latency);
  hist_add(&cm->latency_send, send_latency);

  if (class_index == BK_CLASS_MUST_DELIVER &&
      sched_latency <= m->cfg.deadline_ns) {
    cm->counts.deadline_hit++;
  }

  if (h->origin_id < m->cfg.max_origin_id) {
    bk_latest *latest =
        &m->latest[(size_t)h->origin_id * BK_N_CLASSES + class_index];
    if (!latest->has || h->sched_ts_ns > latest->sched_ts_ns) {
      latest->has = true;
      latest->sched_ts_ns = h->sched_ts_ns;
    }
  }
}

void bk_metrics_tick(bk_metrics *m, uint64_t now_ns) {
  if (m == NULL) {
    return;
  }
  if (m->next_staleness_sample_ns == 0) {
    m->next_staleness_sample_ns = now_ns;
  }

  while (m->next_staleness_sample_ns <= now_ns) {
    const uint64_t sample_ns = m->next_staleness_sample_ns;
    for (uint32_t origin = 0; origin < m->cfg.max_origin_id; ++origin) {
      const bk_latest *latest =
          &m->latest[(size_t)origin * BK_N_CLASSES + BK_CLASS_LOSS_TOLERANT];
      if (!latest->has) {
        continue;
      }
      hist_add(&m->staleness,
               bk_saturating_sub_u64(sample_ns, latest->sched_ts_ns));
    }
    if (UINT64_MAX - m->next_staleness_sample_ns <
        m->cfg.staleness_period_ns) {
      m->next_staleness_sample_ns = UINT64_MAX;
      break;
    }
    m->next_staleness_sample_ns += m->cfg.staleness_period_ns;
  }
}

void bk_metrics_counts(const bk_metrics *m, bool must_deliver,
                       bk_class_counts *out) {
  if (out == NULL) {
    return;
  }
  memset(out, 0, sizeof(*out));
  if (m == NULL) {
    return;
  }
  const int class_index = must_deliver ? BK_CLASS_MUST_DELIVER
                                       : BK_CLASS_LOSS_TOLERANT;
  *out = m->classes[class_index].counts;
}

uint64_t bk_metrics_staleness_pctl(const bk_metrics *m, double p) {
  if (m == NULL) {
    return 0;
  }
  return hist_percentile(&m->staleness, p);
}

uint64_t bk_metrics_latency_pctl(const bk_metrics *m, bool must_deliver,
                                 double p) {
  if (m == NULL) {
    return 0;
  }
  const int class_index = must_deliver ? BK_CLASS_MUST_DELIVER
                                       : BK_CLASS_LOSS_TOLERANT;
  return hist_percentile(&m->classes[class_index].latency_sched, p);
}

static int json_hist(FILE *f, const bk_hist *h) {
  if (fprintf(f,
              "{\"scheme\":\"log2x16\",\"min_ns\":%" PRIu64
              ",\"max_ns\":%" PRIu64 ",\"count\":%" PRIu64 ",\"p50_ns\":%"
              PRIu64 ",\"p90_ns\":%" PRIu64 ",\"p99_ns\":%" PRIu64
              ",\"bins\":[",
              (uint64_t)BK_HIST_MIN_NS, (uint64_t)BK_HIST_MAX_NS, h->count,
              hist_percentile(h, 0.50), hist_percentile(h, 0.90),
              hist_percentile(h, 0.99)) < 0) {
    return -1;
  }
  for (unsigned i = 0; i < BK_HIST_BINS; ++i) {
    if (i != 0 && fputc(',', f) == EOF) {
      return -1;
    }
    if (fprintf(f, "%" PRIu64, h->bins[i]) < 0) {
      return -1;
    }
  }
  if (fputs("]}", f) == EOF) {
    return -1;
  }
  return 0;
}

static int json_class(FILE *f, const bk_class_metrics *cm) {
  if (fprintf(f,
              "{\"slots\":%" PRIu64 ",\"slots_broadcast\":%" PRIu64
              ",\"submitted\":%" PRIu64 ",\"delivered_unique\":%" PRIu64
              ",\"duplicates\":%" PRIu64 ",\"deadline_hit\":%" PRIu64
              ",\"latency_sched_ns\":",
              cm->counts.slots, cm->counts.slots_broadcast,
              cm->counts.submitted, cm->counts.delivered_unique,
              cm->counts.duplicates, cm->counts.deadline_hit) < 0) {
    return -1;
  }
  if (json_hist(f, &cm->latency_sched) != 0) {
    return -1;
  }
  if (fputs(",\"latency_send_ns\":", f) == EOF) {
    return -1;
  }
  if (json_hist(f, &cm->latency_send) != 0) {
    return -1;
  }
  if (fputc('}', f) == EOF) {
    return -1;
  }
  return 0;
}

int bk_metrics_dump_json(const bk_metrics *m, const char *path) {
  if (m == NULL || path == NULL) {
    return -1;
  }

  FILE *f = fopen(path, "w");
  if (f == NULL) {
    return -1;
  }

  int rc = 0;
  if (fprintf(f, "{\"version\":1,\"histogram\":{\"scheme\":\"log2x16\","
                 "\"subbins\":%u,\"min_ns\":%" PRIu64
                 ",\"max_ns\":%" PRIu64 "},\"classes\":{\"loss_tolerant\":",
              BK_HIST_SUBBINS, (uint64_t)BK_HIST_MIN_NS,
              (uint64_t)BK_HIST_MAX_NS) < 0) {
    rc = -1;
  }
  if (rc == 0 && json_class(f, &m->classes[BK_CLASS_LOSS_TOLERANT]) != 0) {
    rc = -1;
  }
  if (rc == 0 && fputs(",\"must_deliver\":", f) == EOF) {
    rc = -1;
  }
  if (rc == 0 && json_class(f, &m->classes[BK_CLASS_MUST_DELIVER]) != 0) {
    rc = -1;
  }
  if (rc == 0 && fputs("},\"staleness_ns\":", f) == EOF) {
    rc = -1;
  }
  if (rc == 0 && json_hist(f, &m->staleness) != 0) {
    rc = -1;
  }
  if (rc == 0 &&
      fprintf(f,
              ",\"raw\":{\"slots\":%" PRIu64 ",\"submitted\":%" PRIu64
              ",\"recv_measured\":%" PRIu64 ",\"recv_unmeasured\":%" PRIu64
              "}}\n",
              m->raw_slots, m->raw_submitted, m->raw_recv_measured,
              m->raw_recv_unmeasured) < 0) {
    rc = -1;
  }

  if (fclose(f) != 0) {
    rc = -1;
  }
  return rc;
}
