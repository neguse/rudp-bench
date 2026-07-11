#include "benchkit.h"

#include "fixture_transport.h"
#include "test_common.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(void) {
  enum { kConns = 3, kSlots = 24 };
  const uint64_t period_ns = 1000000ull;
  const bk_metrics_config cfg = {
      .max_origin_id = kConns,
      .deadline_ns = 2 * period_ns,
      .staleness_period_ns = period_ns,
      .max_local_index = kConns,
  };

  fx_transport *t = fx_null_new(kConns);
  CHECK(t != NULL);
  // 実 client と同じく proc で1つの metrics を全 conn が共有する。
  // broadcast の複製は (受信側 local conn) 込みのキーで初観測扱いになる
  // (v2.0 縮小 broadcast で踏んだ「複製が duplicate 扱い」の回帰テスト)。
  bk_metrics *metrics = bk_metrics_new(&cfg);
  CHECK(metrics != NULL);

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
      bk_metrics_on_slot(metrics, &h, true);
      CHECK(fx_transport_send(t, &h, h.send_ts_ns) == 0);
    }

    for (int conn = 0; conn < kConns; ++conn) {
      bk_header got;
      uint64_t recv_ts;
      while (fx_transport_recv(t, conn, seq * period_ns, &got, &recv_ts)) {
        bk_metrics_on_recv(metrics, (uint32_t)conn, &got, recv_ts);
      }
    }
    bk_metrics_tick(metrics, seq * period_ns);
  }

  bk_class_counts loss = {0};
  bk_class_counts must = {0};
  bk_metrics_counts(metrics, false, &loss);
  bk_metrics_counts(metrics, true, &must);

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

  CHECK(bk_metrics_staleness_pctl(metrics, 0.99) <= 2 * period_ns);

  char json_path[128];
  int n = snprintf(json_path, sizeof(json_path),
                   "/tmp/benchkit-metrics-zero-%ld.json", (long)getpid());
  CHECK(n > 0 && (size_t)n < sizeof(json_path));
  CHECK(bk_metrics_dump_json(metrics, json_path) == 0);
  FILE *json = fopen(json_path, "r");
  CHECK(json != NULL);
  char buf[4096];
  const size_t got = fread(buf, 1, sizeof(buf) - 1u, json);
  buf[got] = '\0';
  CHECK(fclose(json) == 0);
  CHECK(strstr(buf, "\"bins\":[") != NULL);
  CHECK(strstr(buf, "\"loss_tolerant\"") != NULL);
  unlink(json_path);

  bk_metrics_free(metrics);
  fx_transport_free(t);

  // legacy client proc は total_conns を知らず、自 proc の origin 上限より
  // 大きい remote origin の broadcast も受信する。aggregate delivery と
  // duplicate 会計は従来どおり落としてはならない。
  const bk_metrics_config legacy_cfg = {
      .max_origin_id = 1,
      .deadline_ns = period_ns,
  };
  bk_metrics *legacy = bk_metrics_new(&legacy_cfg);
  CHECK(legacy != NULL);
  const bk_header remote = {
      .seq = 1,
      .sched_ts_ns = period_ns,
      .send_ts_ns = period_ns,
      .flags = BK_FLAG_MEASURE | BK_FLAG_BROADCAST,
      .origin_id = 7,
  };
  bk_metrics_on_recv(legacy, 5, &remote, 2 * period_ns);
  bk_metrics_on_recv(legacy, 5, &remote, 2 * period_ns);
  bk_class_counts remote_counts = {0};
  bk_metrics_counts(legacy, false, &remote_counts);
  CHECK(remote_counts.delivered_unique == 1);
  CHECK(remote_counts.duplicates == 1);
  bk_metrics_free(legacy);
  return 0;
}
