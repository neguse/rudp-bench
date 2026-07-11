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
  uint32_t origin_id;
  uint32_t local_index;
  uint8_t traffic_id;
  uint8_t direction;
  uint8_t class_index;
  bool used;
  bool expected;
  bool has;
  uint64_t sched_ts_ns;
  // latest-value を前進させた受信の時刻。update gap(事象アライン指標)用
  uint64_t recv_ts_ns;
} bk_latest;

typedef struct {
  uint64_t seq;
  uint32_t origin_id;
  uint32_t local_index;  // 受信側 local conn。broadcast の複製と重複を区別する
  uint8_t traffic_id;
  uint8_t direction;
  uint8_t class_index;
  bool used;
} bk_seen_key;

typedef struct {
  bk_class_counts counts;
  bk_hist latency_sched;
  bk_hist latency_send;
  // update gap: latest-value が前進した受信同士の間隔(事象アライン指標)。
  // class 混合にすると must-deliver の送信間隔(1Hz なら 1s)が p99 を
  // 支配して loss 回復の信号が消えるため、class 別に持つ
  bk_hist update_gap;
} bk_class_metrics;

typedef struct {
  uint8_t traffic_id;
  uint8_t direction;
  uint8_t class_index;
  uint64_t deadline_ns;
  bk_class_metrics metrics;
  bk_hist staleness;
} bk_traffic_metrics;

typedef struct {
  uint8_t traffic_id;
  uint8_t direction;
  uint64_t deadline_ns;
} bk_traffic_deadline;

struct bk_metrics {
  bk_metrics_config cfg;
  bk_class_metrics classes[BK_N_CLASSES];
  bk_hist staleness;
  bk_latest *latest;
  size_t latest_cap;
  size_t latest_len;
  bool legacy_single_latest;
  bk_traffic_metrics *traffic;
  size_t traffic_len;
  size_t traffic_cap;
  bk_traffic_deadline *deadlines;
  size_t deadline_len;
  size_t deadline_cap;
  uint64_t next_staleness_sample_ns;
  bk_seen_key *seen;
  size_t seen_cap;
  size_t seen_len;
  uint64_t raw_slots;
  uint64_t raw_submitted;
  uint64_t raw_recv_measured;
  uint64_t raw_recv_unmeasured;
  uint64_t raw_timestamp_order_violations;
};

static uint64_t hash_mix_u64(uint64_t x) {
  x ^= x >> 33u;
  x *= 0xff51afd7ed558ccdull;
  x ^= x >> 33u;
  x *= 0xc4ceb9fe1a85ec53ull;
  x ^= x >> 33u;
  return x;
}

static uint64_t flow_hash(uint32_t local_index, uint32_t origin_id,
                          uint8_t traffic_id, uint8_t direction,
                          uint8_t class_index) {
  uint64_t x = (uint64_t)origin_id * 0x9e3779b185ebca87ull;
  x ^= (uint64_t)local_index * 0xc2b2ae3d27d4eb4full;
  x ^= (uint64_t)traffic_id << 40u;
  x ^= (uint64_t)direction << 52u;
  x ^= (uint64_t)class_index << 60u;
  return hash_mix_u64(x);
}

static uint64_t seen_hash(uint32_t local_index, uint32_t origin_id,
                          uint8_t traffic_id, uint8_t direction,
                          uint8_t class_index, uint64_t seq) {
  uint64_t x = seq;
  x ^= flow_hash(local_index, origin_id, traffic_id, direction, class_index);
  return hash_mix_u64(x);
}

static bool key_equal(const bk_seen_key *k, uint32_t local_index,
                      uint32_t origin_id, uint8_t traffic_id,
                      uint8_t direction, uint8_t class_index, uint64_t seq) {
  return k->used && k->local_index == local_index &&
         k->origin_id == origin_id && k->traffic_id == traffic_id &&
         k->direction == direction && k->class_index == class_index &&
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
    const uint64_t h =
        seen_hash(old[i].local_index, old[i].origin_id, old[i].traffic_id,
                  old[i].direction, old[i].class_index, old[i].seq);
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
                           uint32_t origin_id, uint8_t traffic_id,
                           uint8_t direction, uint8_t class_index, uint64_t seq,
                           bool *is_new) {
  if ((m->seen_len + 1u) * 10u >= m->seen_cap * 7u) {
    if (seen_rehash(m, m->seen_cap * 2u) != 0) {
      return -1;
    }
  }

  const uint64_t h = seen_hash(local_index, origin_id, traffic_id, direction,
                               class_index, seq);
  size_t pos = (size_t)h & (m->seen_cap - 1u);
  while (m->seen[pos].used) {
    if (key_equal(&m->seen[pos], local_index, origin_id, traffic_id, direction,
                  class_index, seq)) {
      *is_new = false;
      return 0;
    }
    pos = (pos + 1u) & (m->seen_cap - 1u);
  }

  m->seen[pos].used = true;
  m->seen[pos].local_index = local_index;
  m->seen[pos].origin_id = origin_id;
  m->seen[pos].traffic_id = traffic_id;
  m->seen[pos].direction = direction;
  m->seen[pos].class_index = class_index;
  m->seen[pos].seq = seq;
  m->seen_len++;
  *is_new = true;
  return 0;
}

static bool latest_key_equal(const bk_latest *latest, uint32_t local_index,
                             uint32_t origin_id, uint8_t traffic_id,
                             uint8_t direction, uint8_t class_index) {
  return latest->used && latest->local_index == local_index &&
         latest->origin_id == origin_id && latest->traffic_id == traffic_id &&
         latest->direction == direction &&
         latest->class_index == class_index;
}

static int latest_rehash(bk_metrics *m, size_t new_cap) {
  bk_latest *old = m->latest;
  const size_t old_cap = m->latest_cap;
  bk_latest *next = (bk_latest *)calloc(new_cap, sizeof(*next));
  if (next == NULL) {
    return -1;
  }
  m->latest = next;
  m->latest_cap = new_cap;
  m->latest_len = 0;
  for (size_t i = 0; i < old_cap; ++i) {
    if (!old[i].used) {
      continue;
    }
    const uint64_t h =
        flow_hash(old[i].local_index, old[i].origin_id, old[i].traffic_id,
                  old[i].direction, old[i].class_index);
    size_t pos = (size_t)h & (new_cap - 1u);
    while (next[pos].used) {
      pos = (pos + 1u) & (new_cap - 1u);
    }
    next[pos] = old[i];
    m->latest_len++;
  }
  free(old);
  return 0;
}

static bk_latest *latest_get(bk_metrics *m, uint32_t local_index,
                             uint32_t origin_id, uint8_t traffic_id,
                             uint8_t direction, uint8_t class_index,
                             bool create) {
  if (m == NULL || origin_id >= m->cfg.max_origin_id ||
      direction >= BK_N_DIRECTIONS || class_index >= BK_N_CLASSES) {
    return NULL;
  }
  if (m->legacy_single_latest) {
    local_index = 0;
  } else if (local_index >= m->cfg.max_local_index) {
    return NULL;
  }
  if (create && (m->latest_len + 1u) * 10u >= m->latest_cap * 7u &&
      latest_rehash(m, m->latest_cap * 2u) != 0) {
    return NULL;
  }
  const uint64_t h =
      flow_hash(local_index, origin_id, traffic_id, direction, class_index);
  size_t pos = (size_t)h & (m->latest_cap - 1u);
  while (m->latest[pos].used) {
    if (latest_key_equal(&m->latest[pos], local_index, origin_id, traffic_id,
                         direction, class_index)) {
      return &m->latest[pos];
    }
    pos = (pos + 1u) & (m->latest_cap - 1u);
  }
  if (!create) {
    return NULL;
  }
  bk_latest *latest = &m->latest[pos];
  latest->used = true;
  latest->local_index = local_index;
  latest->origin_id = origin_id;
  latest->traffic_id = traffic_id;
  latest->direction = direction;
  latest->class_index = class_index;
  m->latest_len++;
  return latest;
}

static uint64_t traffic_deadline(const bk_metrics *m, uint8_t traffic_id,
                                 uint8_t direction) {
  for (size_t i = 0; i < m->deadline_len; ++i) {
    if (m->deadlines[i].traffic_id == traffic_id &&
        m->deadlines[i].direction == direction) {
      return m->deadlines[i].deadline_ns;
    }
  }
  return m->cfg.deadline_ns;
}

static bk_traffic_metrics *traffic_get(bk_metrics *m, uint8_t traffic_id,
                                       uint8_t direction, uint8_t class_index,
                                       bool create) {
  if (m == NULL || direction >= BK_N_DIRECTIONS ||
      class_index >= BK_N_CLASSES) {
    return NULL;
  }
  for (size_t i = 0; i < m->traffic_len; ++i) {
    bk_traffic_metrics *tm = &m->traffic[i];
    if (tm->traffic_id == traffic_id && tm->direction == direction &&
        tm->class_index == class_index) {
      return tm;
    }
  }
  if (!create) {
    return NULL;
  }
  if (m->traffic_len == m->traffic_cap) {
    const size_t new_cap = m->traffic_cap == 0 ? 8u : m->traffic_cap * 2u;
    bk_traffic_metrics *next =
        (bk_traffic_metrics *)realloc(m->traffic, new_cap * sizeof(*next));
    if (next == NULL) {
      return NULL;
    }
    m->traffic = next;
    m->traffic_cap = new_cap;
  }
  bk_traffic_metrics *tm = &m->traffic[m->traffic_len++];
  memset(tm, 0, sizeof(*tm));
  tm->traffic_id = traffic_id;
  tm->direction = direction;
  tm->class_index = class_index;
  tm->deadline_ns = traffic_deadline(m, traffic_id, direction);
  return tm;
}

static const bk_traffic_metrics *traffic_find(const bk_metrics *m,
                                              uint8_t traffic_id,
                                              uint8_t direction,
                                              uint8_t class_index) {
  if (m == NULL || direction >= BK_N_DIRECTIONS ||
      class_index >= BK_N_CLASSES) {
    return NULL;
  }
  for (size_t i = 0; i < m->traffic_len; ++i) {
    const bk_traffic_metrics *tm = &m->traffic[i];
    if (tm->traffic_id == traffic_id && tm->direction == direction &&
        tm->class_index == class_index) {
      return tm;
    }
  }
  return NULL;
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
  const bool legacy_single_latest = local.max_local_index == 0;
  if (legacy_single_latest) {
    local.max_local_index = 1;
  }
  if (local.staleness_period_ns == 0) {
    local.staleness_period_ns = BK_DEFAULT_STALENESS_PERIOD_NS;
  }

  bk_metrics *m = (bk_metrics *)calloc(1, sizeof(*m));
  if (m == NULL) {
    return NULL;
  }
  m->cfg = local;
  m->legacy_single_latest = legacy_single_latest;
  m->latest_cap = 1024u;
  m->latest = (bk_latest *)calloc(m->latest_cap, sizeof(*m->latest));
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
  free(m->traffic);
  free(m->deadlines);
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
  const uint8_t direction = (uint8_t)BK_FLAGS_DIRECTION(h->flags);
  bk_class_metrics *aggregate = &m->classes[class_index];
  bk_traffic_metrics *traffic =
      traffic_get(m, h->traffic_id, direction, (uint8_t)class_index, true);
  aggregate->counts.slots++;
  if (traffic != NULL) {
    traffic->metrics.counts.slots++;
  }
  if ((h->flags & BK_FLAG_BROADCAST) != 0) {
    aggregate->counts.slots_broadcast++;
    if (traffic != NULL) {
      traffic->metrics.counts.slots_broadcast++;
    }
  }
  if (submitted) {
    aggregate->counts.submitted++;
    if (traffic != NULL) {
      traffic->metrics.counts.submitted++;
    }
  }
}

int bk_metrics_expect_latest(bk_metrics *m, uint32_t local_index,
                             uint32_t origin_id, uint8_t traffic_id,
                             bk_direction direction,
                             uint64_t first_sched_ts_ns) {
  bk_latest *latest = latest_get(m, local_index, origin_id, traffic_id,
                                 (uint8_t)direction, BK_CLASS_LOSS_TOLERANT,
                                 true);
  if (latest == NULL) {
    return -1;
  }
  bk_traffic_metrics *traffic =
      traffic_get(m, traffic_id, (uint8_t)direction,
                  BK_CLASS_LOSS_TOLERANT, true);
  if (!latest->expected) {
    m->classes[BK_CLASS_LOSS_TOLERANT].counts.expected_flows++;
    if (traffic != NULL) {
      traffic->metrics.counts.expected_flows++;
    }
  }
  if (!latest->has &&
      (!latest->expected || first_sched_ts_ns < latest->sched_ts_ns)) {
    latest->sched_ts_ns = first_sched_ts_ns;
  }
  latest->expected = true;
  return 0;
}

void bk_metrics_on_recv(bk_metrics *m, uint32_t local_index,
                        const bk_header *h, uint64_t recv_ts_ns) {
  if (m == NULL || h == NULL) {
    return;
  }
  const uint8_t direction = (uint8_t)BK_FLAGS_DIRECTION(h->flags);
  if (direction >= BK_N_DIRECTIONS ||
      (!m->legacy_single_latest && local_index >= m->cfg.max_local_index)) {
    return;
  }
  if ((h->flags & BK_FLAG_MEASURE) == 0) {
    m->raw_recv_unmeasured++;
    return;
  }

  m->raw_recv_measured++;
  const uint8_t class_index = (uint8_t)bk_class_index_from_flags(h->flags);
  bk_class_metrics *aggregate = &m->classes[class_index];
  bk_traffic_metrics *traffic =
      traffic_get(m, h->traffic_id, direction, class_index, true);
  bool is_new = false;
  if (seen_insert_new(m, local_index, h->origin_id, h->traffic_id, direction,
                      class_index, h->seq, &is_new) != 0) {
    return;
  }
  if (!is_new) {
    aggregate->counts.duplicates++;
    if (traffic != NULL) {
      traffic->metrics.counts.duplicates++;
    }
    return;
  }

  aggregate->counts.delivered_unique++;
  if (traffic != NULL) {
    traffic->metrics.counts.delivered_unique++;
  }
  if (h->sched_ts_ns > h->send_ts_ns || h->send_ts_ns > recv_ts_ns) {
    m->raw_timestamp_order_violations++;
  }
  const uint64_t sched_latency =
      bk_saturating_sub_u64(recv_ts_ns, h->sched_ts_ns);
  const uint64_t send_latency =
      bk_saturating_sub_u64(recv_ts_ns, h->send_ts_ns);
  hist_add(&aggregate->latency_sched, sched_latency);
  hist_add(&aggregate->latency_send, send_latency);
  if (traffic != NULL) {
    hist_add(&traffic->metrics.latency_sched, sched_latency);
    hist_add(&traffic->metrics.latency_send, send_latency);
  }

  if (class_index == BK_CLASS_MUST_DELIVER) {
    const uint64_t resolved_deadline =
        traffic != NULL ? traffic->deadline_ns : m->cfg.deadline_ns;
    if (sched_latency <= resolved_deadline) {
      aggregate->counts.deadline_hit++;
      if (traffic != NULL) {
        traffic->metrics.counts.deadline_hit++;
      }
    }
  }

  bk_latest *latest =
      latest_get(m, local_index, h->origin_id, h->traffic_id, direction,
                 class_index, true);
  if (latest != NULL) {
    if (!latest->expected) {
      aggregate->counts.expected_flows++;
      if (traffic != NULL) {
        traffic->metrics.counts.expected_flows++;
      }
    }
    if (!latest->has) {
      aggregate->counts.observed_flows++;
      if (traffic != NULL) {
        traffic->metrics.counts.observed_flows++;
      }
    }
    if (!latest->has || h->sched_ts_ns > latest->sched_ts_ns) {
      // update gap: latest-value を前進させた受信同士の間隔(事象アライン)。
      // 古い並べ替え到着は latest を進めないので gap にも数えない
      if (latest->has) {
        const uint64_t gap =
            bk_saturating_sub_u64(recv_ts_ns, latest->recv_ts_ns);
        hist_add(&aggregate->update_gap, gap);
        if (traffic != NULL) {
          hist_add(&traffic->metrics.update_gap, gap);
        }
      }
      latest->expected = true;
      latest->has = true;
      latest->sched_ts_ns = h->sched_ts_ns;
      latest->recv_ts_ns = recv_ts_ns;
    }
  }
}

static bk_class_counts finalized_counts(bk_class_counts counts) {
  counts.never_received_flows =
      counts.expected_flows >= counts.observed_flows
          ? counts.expected_flows - counts.observed_flows
          : 0;
  return counts;
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
    for (size_t i = 0; i < m->latest_cap; ++i) {
      const bk_latest *latest = &m->latest[i];
      if (!latest->used || latest->class_index != BK_CLASS_LOSS_TOLERANT ||
          (!latest->expected && !latest->has)) {
        continue;
      }
      const uint64_t age =
          bk_saturating_sub_u64(sample_ns, latest->sched_ts_ns);
      hist_add(&m->staleness, age);
      bk_traffic_metrics *traffic =
          traffic_get(m, latest->traffic_id, latest->direction,
                      BK_CLASS_LOSS_TOLERANT, true);
      if (traffic != NULL) {
        hist_add(&traffic->staleness, age);
      }
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
  *out = finalized_counts(m->classes[class_index].counts);
}

uint64_t bk_metrics_staleness_pctl(const bk_metrics *m, double p) {
  if (m == NULL) {
    return 0;
  }
  return hist_percentile(&m->staleness, p);
}

uint64_t bk_metrics_update_gap_pctl(const bk_metrics *m, bool must_deliver,
                                    double p) {
  if (m == NULL) {
    return 0;
  }
  const int class_index = must_deliver ? BK_CLASS_MUST_DELIVER
                                       : BK_CLASS_LOSS_TOLERANT;
  return hist_percentile(&m->classes[class_index].update_gap, p);
}

void bk_metrics_raw_counts(const bk_metrics *m, uint64_t *slots,
                           uint64_t *submitted, uint64_t *recv_measured,
                           uint64_t *recv_unmeasured) {
  if (slots != NULL) {
    *slots = (m != NULL) ? m->raw_slots : 0;
  }
  if (submitted != NULL) {
    *submitted = (m != NULL) ? m->raw_submitted : 0;
  }
  if (recv_measured != NULL) {
    *recv_measured = (m != NULL) ? m->raw_recv_measured : 0;
  }
  if (recv_unmeasured != NULL) {
    *recv_unmeasured = (m != NULL) ? m->raw_recv_unmeasured : 0;
  }
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

int bk_metrics_traffic_counts(const bk_metrics *m, uint8_t traffic_id,
                              bk_direction direction, bool must_deliver,
                              bk_class_counts *out) {
  if (out == NULL) {
    return -1;
  }
  memset(out, 0, sizeof(*out));
  const uint8_t class_index =
      must_deliver ? BK_CLASS_MUST_DELIVER : BK_CLASS_LOSS_TOLERANT;
  const bk_traffic_metrics *traffic =
      traffic_find(m, traffic_id, (uint8_t)direction, class_index);
  if (traffic == NULL) {
    return -1;
  }
  *out = finalized_counts(traffic->metrics.counts);
  return 0;
}

uint64_t bk_metrics_traffic_staleness_pctl(const bk_metrics *m,
                                           uint8_t traffic_id,
                                           bk_direction direction, double p) {
  const bk_traffic_metrics *traffic = traffic_find(
      m, traffic_id, (uint8_t)direction, BK_CLASS_LOSS_TOLERANT);
  return traffic == NULL ? 0 : hist_percentile(&traffic->staleness, p);
}

uint64_t bk_metrics_traffic_update_gap_pctl(const bk_metrics *m,
                                            uint8_t traffic_id,
                                            bk_direction direction,
                                            bool must_deliver, double p) {
  const uint8_t class_index =
      must_deliver ? BK_CLASS_MUST_DELIVER : BK_CLASS_LOSS_TOLERANT;
  const bk_traffic_metrics *traffic =
      traffic_find(m, traffic_id, (uint8_t)direction, class_index);
  return traffic == NULL
             ? 0
             : hist_percentile(&traffic->metrics.update_gap, p);
}

uint64_t bk_metrics_traffic_latency_pctl(const bk_metrics *m,
                                         uint8_t traffic_id,
                                         bk_direction direction,
                                         bool must_deliver, double p) {
  const uint8_t class_index =
      must_deliver ? BK_CLASS_MUST_DELIVER : BK_CLASS_LOSS_TOLERANT;
  const bk_traffic_metrics *traffic =
      traffic_find(m, traffic_id, (uint8_t)direction, class_index);
  return traffic == NULL
             ? 0
             : hist_percentile(&traffic->metrics.latency_sched, p);
}

int bk_metrics_set_traffic_deadline(bk_metrics *m, uint8_t traffic_id,
                                    bk_direction direction,
                                    uint64_t deadline_ns) {
  if (m == NULL || direction < 0 || direction >= BK_N_DIRECTIONS) {
    return -1;
  }
  for (size_t i = 0; i < m->deadline_len; ++i) {
    if (m->deadlines[i].traffic_id == traffic_id &&
        m->deadlines[i].direction == (uint8_t)direction) {
      m->deadlines[i].deadline_ns = deadline_ns;
      for (size_t j = 0; j < m->traffic_len; ++j) {
        if (m->traffic[j].traffic_id == traffic_id &&
            m->traffic[j].direction == (uint8_t)direction) {
          m->traffic[j].deadline_ns = deadline_ns;
        }
      }
      return 0;
    }
  }
  if (m->deadline_len == m->deadline_cap) {
    const size_t new_cap = m->deadline_cap == 0 ? 4u : m->deadline_cap * 2u;
    bk_traffic_deadline *next = (bk_traffic_deadline *)realloc(
        m->deadlines, new_cap * sizeof(*next));
    if (next == NULL) {
      return -1;
    }
    m->deadlines = next;
    m->deadline_cap = new_cap;
  }
  m->deadlines[m->deadline_len++] = (bk_traffic_deadline){
      .traffic_id = traffic_id,
      .direction = (uint8_t)direction,
      .deadline_ns = deadline_ns,
  };
  for (size_t j = 0; j < m->traffic_len; ++j) {
    if (m->traffic[j].traffic_id == traffic_id &&
        m->traffic[j].direction == (uint8_t)direction) {
      m->traffic[j].deadline_ns = deadline_ns;
    }
  }
  return 0;
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
  const bk_class_counts counts = finalized_counts(cm->counts);
  if (fprintf(f,
              "{\"slots\":%" PRIu64 ",\"slots_broadcast\":%" PRIu64
              ",\"submitted\":%" PRIu64 ",\"delivered_unique\":%" PRIu64
              ",\"duplicates\":%" PRIu64 ",\"deadline_hit\":%" PRIu64
              ",\"expected_flows\":%" PRIu64
              ",\"observed_flows\":%" PRIu64
              ",\"never_received_flows\":%" PRIu64
              ",\"latency_sched_ns\":",
              counts.slots, counts.slots_broadcast, counts.submitted,
              counts.delivered_unique, counts.duplicates, counts.deadline_hit,
              counts.expected_flows, counts.observed_flows,
              counts.never_received_flows) < 0) {
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
  if (fputs(",\"update_gap_ns\":", f) == EOF) {
    return -1;
  }
  if (json_hist(f, &cm->update_gap) != 0) {
    return -1;
  }
  if (fputc('}', f) == EOF) {
    return -1;
  }
  return 0;
}

static const char *direction_name(uint8_t direction) {
  switch (direction) {
    case BK_DIRECTION_ROOM_RELAY:
      return "room_relay";
    case BK_DIRECTION_CLIENT_TO_SERVER:
      return "client_to_server";
    case BK_DIRECTION_SERVER_TO_CLIENT:
      return "server_to_client";
    default:
      return "invalid";
  }
}

static int json_traffic(FILE *f, const bk_traffic_metrics *tm) {
  const bk_class_counts counts = finalized_counts(tm->metrics.counts);
  const char *class_name = tm->class_index == BK_CLASS_MUST_DELIVER
                               ? "must_deliver"
                               : "loss_tolerant";
  if (fprintf(f,
              "{\"traffic_id\":%u,\"direction\":\"%s\",\"class\":\"%s\""
              ",\"deadline_ns\":%" PRIu64
              ",\"slots\":%" PRIu64 ",\"slots_broadcast\":%" PRIu64
              ",\"submitted\":%" PRIu64 ",\"delivered_unique\":%" PRIu64
              ",\"duplicates\":%" PRIu64 ",\"deadline_hit\":%" PRIu64
              ",\"expected_flows\":%" PRIu64
              ",\"observed_flows\":%" PRIu64
              ",\"never_received_flows\":%" PRIu64
              ",\"latency_sched_ns\":",
              (unsigned)tm->traffic_id, direction_name(tm->direction),
              class_name, tm->deadline_ns, counts.slots,
              counts.slots_broadcast, counts.submitted,
              counts.delivered_unique, counts.duplicates, counts.deadline_hit,
              counts.expected_flows, counts.observed_flows,
              counts.never_received_flows) < 0) {
    return -1;
  }
  if (json_hist(f, &tm->metrics.latency_sched) != 0 ||
      fputs(",\"latency_send_ns\":", f) == EOF ||
      json_hist(f, &tm->metrics.latency_send) != 0 ||
      fputs(",\"update_gap_ns\":", f) == EOF ||
      json_hist(f, &tm->metrics.update_gap) != 0 ||
      fputs(",\"staleness_ns\":", f) == EOF ||
      json_hist(f, &tm->staleness) != 0 || fputc('}', f) == EOF) {
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
  if (fprintf(f, "{\"version\":2,\"histogram\":{\"scheme\":\"log2x16\","
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
  if (rc == 0 && fputs("},\"traffic\":[", f) == EOF) {
    rc = -1;
  }
  for (size_t i = 0; rc == 0 && i < m->traffic_len; ++i) {
    if ((i != 0 && fputc(',', f) == EOF) ||
        json_traffic(f, &m->traffic[i]) != 0) {
      rc = -1;
    }
  }
  if (rc == 0 && fputs("],\"staleness_ns\":", f) == EOF) {
    rc = -1;
  }
  if (rc == 0 && json_hist(f, &m->staleness) != 0) {
    rc = -1;
  }
  if (rc == 0 &&
      fprintf(f,
              ",\"raw\":{\"slots\":%" PRIu64 ",\"submitted\":%" PRIu64
              ",\"recv_measured\":%" PRIu64 ",\"recv_unmeasured\":%" PRIu64
              ",\"timestamp_order_violations\":%" PRIu64
              "}}\n",
              m->raw_slots, m->raw_submitted, m->raw_recv_measured,
              m->raw_recv_unmeasured, m->raw_timestamp_order_violations) < 0) {
    rc = -1;
  }

  if (fclose(f) != 0) {
    rc = -1;
  }
  return rc;
}
