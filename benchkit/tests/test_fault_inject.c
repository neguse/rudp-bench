#include "benchkit.h"

#include "fixture_transport.h"
#include "test_common.h"

#include <stdint.h>

int main(void) {
  enum { kSlots = 55 };
  const uint64_t period_ns = 1000000ull;
  const uint64_t delay_ns = 20000000ull;
  const bk_metrics_config cfg = {
      .max_origin_id = 1,
      .deadline_ns = 100000000ull,
      .staleness_period_ns = period_ns,
  };

  fx_transport *t = fx_fault_inject_new(1, delay_ns);
  CHECK(t != NULL);
  bk_metrics *m = bk_metrics_new(&cfg);
  CHECK(m != NULL);

  for (uint64_t seq = 1; seq <= kSlots; ++seq) {
    bk_header h = {
        .seq = seq,
        .sched_ts_ns = seq * period_ns,
        .send_ts_ns = seq * period_ns,
        .flags = BK_FLAG_MEASURE,
        .origin_id = 0,
    };
    bk_metrics_on_slot(m, &h, true);
    CHECK(fx_transport_send(t, &h, h.send_ts_ns) == 0);
  }

  const uint64_t drain_until = (uint64_t)kSlots * period_ns + delay_ns * 2u;
  bk_header got;
  uint64_t recv_ts;
  while (fx_transport_recv(t, 0, drain_until, &got, &recv_ts)) {
    bk_metrics_on_recv(m, 0, &got, recv_ts);
  }

  bk_class_counts loss;
  bk_metrics_counts(m, false, &loss);
  CHECK(loss.slots == kSlots);
  CHECK(loss.delivered_unique == kSlots);
  CHECK(loss.duplicates == fx_transport_injected_duplicates(t));
  CHECK(loss.duplicates == kSlots / 7);
  CHECK(bk_metrics_latency_pctl(m, false, 0.95) >= delay_ns);

  bk_metrics_free(m);
  fx_transport_free(t);
  return 0;
}
