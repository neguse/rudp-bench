#include "benchkit.h"

#include "test_common.h"

#include <stdint.h>

int main(void) {
  const bk_stream streams[] = {
      {.must_deliver = false,
       .broadcast = false,
       .traffic_id = 1,
       .direction = BK_DIRECTION_CLIENT_TO_SERVER,
       .interval_ns = 100},
      {.must_deliver = true,
       .broadcast = true,
       .traffic_id = 2,
       .direction = BK_DIRECTION_SERVER_TO_CLIENT,
       .interval_ns = 250},
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
  CHECK(BK_FLAGS_DIRECTION(s.flags) == BK_DIRECTION_CLIENT_TO_SERVER);
  CHECK(s.traffic_id == 1);

  CHECK(bk_plan_next(p, 1000, &s));
  CHECK(s.stream_index == 1);
  CHECK(s.seq == 1);
  CHECK(s.sched_ts_ns == 1000);
  CHECK((s.flags & BK_FLAG_MUST_DELIVER) != 0);
  CHECK((s.flags & BK_FLAG_BROADCAST) != 0);
  CHECK((s.flags & BK_FLAG_MEASURE) == 0);
  CHECK(BK_FLAGS_DIRECTION(s.flags) == BK_DIRECTION_SERVER_TO_CLIENT);
  CHECK(s.traffic_id == 2);
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

  const bk_stream invalid = {
      .direction = (bk_direction)3,
      .interval_ns = 1,
  };
  CHECK(bk_plan_new(&invalid, 1, 0, 0, 1) == NULL);
  return 0;
}
