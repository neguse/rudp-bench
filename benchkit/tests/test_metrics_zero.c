#include "benchkit.h"

#include "fixture_transport.h"
#include "test_common.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void add_counts(bk_class_counts *dst, const bk_class_counts *src) {
  dst->slots += src->slots;
  dst->slots_broadcast += src->slots_broadcast;
  dst->submitted += src->submitted;
  dst->delivered_unique += src->delivered_unique;
  dst->duplicates += src->duplicates;
  dst->deadline_hit += src->deadline_hit;
}

int main(void) {
  enum { kConns = 3, kSlots = 24 };
  const uint64_t period_ns = 1000000ull;
  const bk_metrics_config cfg = {
      .max_origin_id = kConns,
      .deadline_ns = 2 * period_ns,
      .staleness_period_ns = period_ns,
  };

  fx_transport *t = fx_null_new(kConns);
  CHECK(t != NULL);
  bk_metrics *metrics[kConns];
  for (int i = 0; i < kConns; ++i) {
    metrics[i] = bk_metrics_new(&cfg);
    CHECK(metrics[i] != NULL);
  }

  for (uint64_t seq = 1; seq <= kSlots; ++seq) {
    for (int origin = 0; origin < kConns; ++origin) {
      const bool broadcast = (seq % 2u) == 0u;
      bk_header h = {
          .seq = seq,
          .sched_ts_ns = seq * period_ns,
          .send_ts_ns = seq * period_ns,
          .flags = BK_FLAG_MEASURE |
                   (broadcast ? BK_FLAG_BROADCAST : BK_FLAG_MUST_DELIVER),
          .origin_id = (uint32_t)origin,
      };
      bk_metrics_on_slot(metrics[origin], &h, true);
      CHECK(fx_transport_send(t, &h, h.send_ts_ns) == 0);
    }

    for (int conn = 0; conn < kConns; ++conn) {
      bk_header got;
      uint64_t recv_ts;
      while (fx_transport_recv(t, conn, seq * period_ns, &got, &recv_ts)) {
        bk_metrics_on_recv(metrics[conn], &got, recv_ts);
      }
      bk_metrics_tick(metrics[conn], seq * period_ns);
    }
  }

  bk_class_counts loss = {0};
  bk_class_counts must = {0};
  for (int i = 0; i < kConns; ++i) {
    bk_class_counts c;
    bk_metrics_counts(metrics[i], false, &c);
    add_counts(&loss, &c);
    bk_metrics_counts(metrics[i], true, &c);
    add_counts(&must, &c);
  }

  // 期待受信数は orchestrator と同じ式で外側が計算する(benchkit は生カウントのみ):
  // expected = (slots - slots_broadcast) + slots_broadcast × 接続数
  const uint64_t loss_expected =
      (loss.slots - loss.slots_broadcast) + loss.slots_broadcast * kConns;
  const uint64_t must_expected =
      (must.slots - must.slots_broadcast) + must.slots_broadcast * kConns;
  CHECK(loss.slots > 0);
  CHECK(must.slots > 0);
  CHECK(loss.slots_broadcast == loss.slots);  // このテストの loss は全 broadcast
  CHECK(must.slots_broadcast == 0);           // must は全 echo
  CHECK(loss.delivered_unique == loss_expected);
  CHECK(must.delivered_unique == must_expected);
  CHECK(loss.duplicates == 0);
  CHECK(must.duplicates == 0);
  CHECK(must.deadline_hit == must_expected);

  for (int i = 0; i < kConns; ++i) {
    CHECK(bk_metrics_staleness_pctl(metrics[i], 0.99) <= 2 * period_ns);
  }

  char json_path[128];
  int n = snprintf(json_path, sizeof(json_path),
                   "/tmp/benchkit-metrics-zero-%ld.json", (long)getpid());
  CHECK(n > 0 && (size_t)n < sizeof(json_path));
  CHECK(bk_metrics_dump_json(metrics[0], json_path) == 0);
  FILE *json = fopen(json_path, "r");
  CHECK(json != NULL);
  char buf[4096];
  const size_t got = fread(buf, 1, sizeof(buf) - 1u, json);
  buf[got] = '\0';
  CHECK(fclose(json) == 0);
  CHECK(strstr(buf, "\"bins\":[") != NULL);
  CHECK(strstr(buf, "\"loss_tolerant\"") != NULL);
  unlink(json_path);

  for (int i = 0; i < kConns; ++i) {
    bk_metrics_free(metrics[i]);
  }
  fx_transport_free(t);
  return 0;
}
