#include "benchkit.h"

#include "test_common.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *read_file(const char *path) {
  FILE *f = fopen(path, "r");
  CHECK(f != NULL);
  CHECK(fseek(f, 0, SEEK_END) == 0);
  const long size = ftell(f);
  CHECK(size >= 0);
  CHECK(fseek(f, 0, SEEK_SET) == 0);
  char *data = (char *)malloc((size_t)size + 1u);
  CHECK(data != NULL);
  CHECK(fread(data, 1, (size_t)size, f) == (size_t)size);
  data[size] = '\0';
  CHECK(fclose(f) == 0);
  return data;
}

int main(void) {
  enum {
    kClients = 2,
    kServerOrigin = kClients,
    kInputTraffic = BK_TRAFFIC_ID_AUTHORITATIVE_INPUT,
    kStateTraffic = BK_TRAFFIC_ID_AUTHORITATIVE_STATE,
  };
  const uint64_t period_ns = 10000000ull;
  const bk_metrics_config cfg = {
      .max_origin_id = kClients + 1,
      .deadline_ns = 100000000ull,
      .staleness_period_ns = period_ns,
      .max_local_index = kClients,
  };
  bk_metrics *server = bk_metrics_new(&cfg);
  bk_metrics *client = bk_metrics_new(&cfg);
  CHECK(server != NULL);
  CHECK(client != NULL);

  // Input は server が sink として受信し、state とは traffic/direction
  // で別 series になる。series 固有 deadline も legacy global と独立。
  CHECK(bk_metrics_set_traffic_deadline(
            server, kInputTraffic, BK_DIRECTION_CLIENT_TO_SERVER,
            5 * period_ns / 10) == 0);
  const bk_header input = {
      .seq = 1,
      .sched_ts_ns = 100000000ull,
      .send_ts_ns = 100000000ull,
      .flags = BK_FLAG_MEASURE | BK_FLAG_MUST_DELIVER |
               BK_FLAG_DIRECTION(BK_DIRECTION_CLIENT_TO_SERVER),
      .origin_id = 0,
      .traffic_id = kInputTraffic,
  };
  bk_metrics_on_recv(server, 0, &input, input.sched_ts_ns + period_ns);
  // dedup key は direction も含む。他のフィールドが同じでも
  // server-to-client は独立な初観測になる。
  bk_header opposite = input;
  opposite.flags =
      (uint8_t)((opposite.flags & ~BK_FLAG_DIRECTION_MASK) |
                BK_FLAG_DIRECTION(BK_DIRECTION_SERVER_TO_CLIENT));
  bk_metrics_on_recv(server, 0, &opposite,
                     opposite.sched_ts_ns + period_ns);
  bk_header other_traffic = input;
  other_traffic.traffic_id = kStateTraffic;
  bk_metrics_on_recv(server, 0, &other_traffic,
                     other_traffic.sched_ts_ns + period_ns);
  bk_metrics_on_recv(server, kClients, &input,
                     input.sched_ts_ns + period_ns);

  bk_class_counts input_counts = {0};
  CHECK(bk_metrics_traffic_counts(server, kInputTraffic,
                                  BK_DIRECTION_CLIENT_TO_SERVER, true,
                                  &input_counts) == 0);
  CHECK(input_counts.delivered_unique == 1);
  CHECK(input_counts.deadline_hit == 0);
  bk_class_counts opposite_counts = {0};
  CHECK(bk_metrics_traffic_counts(server, kInputTraffic,
                                  BK_DIRECTION_SERVER_TO_CLIENT, true,
                                  &opposite_counts) == 0);
  CHECK(opposite_counts.delivered_unique == 1);
  bk_class_counts other_traffic_counts = {0};
  CHECK(bk_metrics_traffic_counts(server, kStateTraffic,
                                  BK_DIRECTION_CLIENT_TO_SERVER, true,
                                  &other_traffic_counts) == 0);
  CHECK(other_traffic_counts.delivered_unique == 1);
  bk_class_counts aggregate_must = {0};
  bk_metrics_counts(server, true, &aggregate_must);
  CHECK(aggregate_must.delivered_unique == 3);
  CHECK(aggregate_must.deadline_hit == 2);
  CHECK(aggregate_must.deadline_hit == input_counts.deadline_hit +
                                           opposite_counts.deadline_hit +
                                           other_traffic_counts.deadline_hit);

  // authoritative server は global tick plan を1つ持ち、各 tick を frozen
  // roster の各 target へ展開する。logical slot は target ごと。
  const bk_stream state_stream = {
      .must_deliver = false,
      .broadcast = false,
      .traffic_id = kStateTraffic,
      .direction = BK_DIRECTION_SERVER_TO_CLIENT,
      .interval_ns = 10 * period_ns,
  };
  bk_plan *plan = bk_plan_new(&state_stream, 1, 100000000ull, 100000000ull,
                              300000000ull);
  CHECK(plan != NULL);
  bk_slot first;
  bk_slot second;
  CHECK(bk_plan_next(plan, 100000000ull, &first));
  CHECK(bk_plan_next(plan, 200000000ull, &second));
  CHECK(first.seq == 1 && second.seq == 2);
  CHECK(first.traffic_id == kStateTraffic);
  CHECK(BK_FLAGS_DIRECTION(first.flags) == BK_DIRECTION_SERVER_TO_CLIENT);

  bk_header state1 = {
      .seq = first.seq,
      .sched_ts_ns = first.sched_ts_ns,
      .send_ts_ns = first.sched_ts_ns,
      .flags = first.flags,
      .origin_id = kServerOrigin,
      .traffic_id = first.traffic_id,
  };
  bk_header state2 = state1;
  state2.seq = second.seq;
  state2.sched_ts_ns = second.sched_ts_ns;
  state2.send_ts_ns = second.sched_ts_ns;
  for (int target = 0; target < kClients; ++target) {
    bk_metrics_on_slot(server, &state1, true);
    bk_metrics_on_slot(server, &state2, target == 0);
  }

  CHECK(bk_metrics_expect_latest(client, 0, kServerOrigin, kStateTraffic,
                                 BK_DIRECTION_SERVER_TO_CLIENT,
                                 state1.sched_ts_ns) == 0);
  CHECK(bk_metrics_expect_latest(client, 1, kServerOrigin, kStateTraffic,
                                 BK_DIRECTION_SERVER_TO_CLIENT,
                                 state1.sched_ts_ns) == 0);
  // 再登録は expected flow を二重計上しない。deadline setter は
  // expect_latest が series を作った後でも、受信開始前なら反映される。
  CHECK(bk_metrics_expect_latest(client, 1, kServerOrigin, kStateTraffic,
                                 BK_DIRECTION_SERVER_TO_CLIENT,
                                 state1.sched_ts_ns) == 0);
  CHECK(bk_metrics_set_traffic_deadline(
            client, kStateTraffic, BK_DIRECTION_SERVER_TO_CLIENT,
            7 * period_ns) == 0);
  CHECK(bk_metrics_expect_latest(client, kClients, kServerOrigin,
                                 kStateTraffic,
                                 BK_DIRECTION_SERVER_TO_CLIENT,
                                 state1.sched_ts_ns) == -1);

  for (uint64_t now = 100000000ull; now <= 300000000ull;
       now += period_ns) {
    if (now == 110000000ull) {
      bk_metrics_on_recv(client, 0, &state1, now);
    }
    if (now == 210000000ull) {
      bk_metrics_on_recv(client, 0, &state2, now);
    }
    bk_metrics_tick(client, now);
  }

  bk_class_counts state_sent = {0};
  CHECK(bk_metrics_traffic_counts(server, kStateTraffic,
                                  BK_DIRECTION_SERVER_TO_CLIENT, false,
                                  &state_sent) == 0);
  CHECK(state_sent.slots == 4);
  CHECK(state_sent.submitted == 3);
  CHECK(state_sent.slots_broadcast == 0);

  bk_class_counts state_recv = {0};
  CHECK(bk_metrics_traffic_counts(client, kStateTraffic,
                                  BK_DIRECTION_SERVER_TO_CLIENT, false,
                                  &state_recv) == 0);
  CHECK(state_recv.slots == 0);
  CHECK(state_recv.delivered_unique == 2);
  CHECK(state_recv.expected_flows == 2);
  CHECK(state_recv.observed_flows == 1);
  CHECK(state_recv.never_received_flows == 1);
  CHECK(bk_metrics_traffic_staleness_pctl(
            client, kStateTraffic, BK_DIRECTION_SERVER_TO_CLIENT, 0.99) >=
        150000000ull);

  char server_path[128];
  char client_path[128];
  CHECK(snprintf(server_path, sizeof(server_path),
                 "/tmp/benchkit-authoritative-server-%ld.json",
                 (long)getpid()) > 0);
  CHECK(snprintf(client_path, sizeof(client_path),
                 "/tmp/benchkit-authoritative-client-%ld.json",
                 (long)getpid()) > 0);
  CHECK(bk_metrics_dump_json(server, server_path) == 0);
  CHECK(bk_metrics_dump_json(client, client_path) == 0);
  char *server_json = read_file(server_path);
  char *client_json = read_file(client_path);
  CHECK(strstr(server_json, "\"version\":2") != NULL);
  CHECK(strstr(server_json, "\"direction\":\"client_to_server\"") !=
        NULL);
  CHECK(strstr(server_json, "\"direction\":\"server_to_client\"") !=
        NULL);
  CHECK(strstr(server_json, "\"deadline_ns\":5000000") != NULL);
  CHECK(strstr(client_json, "\"deadline_ns\":70000000") != NULL);
  CHECK(strstr(client_json, "\"never_received_flows\":1") != NULL);
  CHECK(strstr(server_json, "\"timestamp_order_violations\":0") != NULL);
  CHECK(strstr(client_json, "\"timestamp_order_violations\":0") != NULL);
  free(server_json);
  free(client_json);

  bk_metrics *order = bk_metrics_new(&cfg);
  CHECK(order != NULL);
  bk_header invalid_order = input;
  invalid_order.seq = 10;
  invalid_order.sched_ts_ns = 300;
  invalid_order.send_ts_ns = 200;
  bk_metrics_on_recv(order, 0, &invalid_order, 400);
  // A duplicate with the same bad timestamps is not another measured unique
  // receive and must not increment the validity counter.
  bk_metrics_on_recv(order, 0, &invalid_order, 400);
  invalid_order.seq = 11;
  invalid_order.sched_ts_ns = 100;
  invalid_order.send_ts_ns = 300;
  bk_metrics_on_recv(order, 0, &invalid_order, 200);
  invalid_order.seq = 12;
  invalid_order.sched_ts_ns = 300;
  invalid_order.send_ts_ns = 200;
  bk_metrics_on_recv(order, 0, &invalid_order, 100);
  CHECK(bk_metrics_dump_json(order, server_path) == 0);
  char *order_json = read_file(server_path);
  CHECK(strstr(order_json, "\"timestamp_order_violations\":3") != NULL);
  free(order_json);
  bk_metrics_free(order);
  unlink(server_path);
  unlink(client_path);

  bk_plan_free(plan);
  bk_metrics_free(client);
  bk_metrics_free(server);
  return 0;
}
