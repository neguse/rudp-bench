#include "benchkit.h"

#include "test_common.h"

#include <stdint.h>

int main(void) {
  const bk_stream streams[] = {
      {.must_deliver = false, .broadcast = false, .interval_ns = 100},
      {.must_deliver = true, .broadcast = true, .interval_ns = 250},
  };
  bk_plan *p = bk_plan_new(streams, 2, 1000, 1200, 1600);
  CHECK(p != NULL);

  bk_slot s;
  CHECK(bk_plan_peek_ns(p) == 1000);
  CHECK(!bk_plan_next(p, 999, &s));

  CHECK(bk_plan_next(p, 1000, &s));
  CHECK(s.stream_index == 0);
  CHECK(s.seq == 1);
  CHECK(s.sched_ts_ns == 1000);
  CHECK((s.flags & BK_FLAG_MEASURE) == 0);

  CHECK(bk_plan_next(p, 1000, &s));
  CHECK(s.stream_index == 1);
  CHECK(s.seq == 1);
  CHECK(s.sched_ts_ns == 1000);
  CHECK((s.flags & BK_FLAG_MUST_DELIVER) != 0);
  CHECK((s.flags & BK_FLAG_BROADCAST) != 0);
  CHECK((s.flags & BK_FLAG_MEASURE) == 0);
  CHECK(bk_plan_peek_ns(p) == 1100);

  CHECK(bk_plan_next(p, 1250, &s));
  CHECK(s.stream_index == 0);
  CHECK(s.seq == 2);
  CHECK(s.sched_ts_ns == 1100);
  CHECK((s.flags & BK_FLAG_MEASURE) == 0);

  CHECK(bk_plan_next(p, 1250, &s));
  CHECK(s.stream_index == 0);
  CHECK(s.seq == 3);
  CHECK(s.sched_ts_ns == 1200);
  CHECK((s.flags & BK_FLAG_MEASURE) != 0);

  CHECK(bk_plan_next(p, 1250, &s));
  CHECK(s.stream_index == 1);
  CHECK(s.seq == 2);
  CHECK(s.sched_ts_ns == 1250);
  CHECK((s.flags & BK_FLAG_MEASURE) != 0);

  uint64_t last_seq0 = 3;
  while (bk_plan_next(p, 1700, &s)) {
    if (s.stream_index == 0) {
      CHECK(s.seq == last_seq0 + 1);
      last_seq0 = s.seq;
    }
    if (s.sched_ts_ns >= 1200 && s.sched_ts_ns < 1600) {
      CHECK((s.flags & BK_FLAG_MEASURE) != 0);
    } else {
      CHECK((s.flags & BK_FLAG_MEASURE) == 0);
    }
    if (s.sched_ts_ns == 1600) {
      CHECK((s.flags & BK_FLAG_MEASURE) == 0);
    }
  }

  CHECK(bk_plan_peek_ns(p) == 1750);
  bk_plan_free(p);
  return 0;
}
