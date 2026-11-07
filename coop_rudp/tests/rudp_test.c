// coop_rudp の送信率と送信待ちの振る舞いを、仮想の時計と回線の上で確かめる。
// 回線は片道 50 ms で、ロス率、ゆらぎ(正規分布)、送る向きごとのボトルネック(速さと待ち行列の上限)を持てる。
#include "coop_rudp/rudp.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
              #cond);                                               \
      exit(1);                                                      \
    }                                                               \
  } while (0)

#define MS 1000000ull
#define MTU 1200u
#define MAX_PKTS 65536
#define CHANNEL_LT 0
#define CHANNEL_MD 1

typedef struct {
  uint64_t at_ns;
  size_t len;
  uint8_t data[MTU];
} pkt;

// 片方向の回線。ボトルネックは送り出しの時刻を速さで並べ、待ち行列の上限を超えた分を捨てる。
// ゆらぎで順序が入れ替わるので、届く時刻を過ぎたものを順不同に取り出す。
typedef struct {
  pkt *q;
  size_t count;
  double loss;
  double jitter_ms;
  uint64_t rate_bytes_per_s;  // 0 なら制限なし
  uint64_t queue_limit_bytes;
  uint64_t busy_until_ns;
} link_t;

typedef struct sim sim_t;

typedef struct {
  sim_t *sim;
  int side;  // 0 = server、1 = client
  rudp_endpoint *ep;
} node_t;

struct sim {
  uint64_t now_ns;
  uint64_t rng;
  link_t links[2];  // links[i] は side i から送る向き
  node_t nodes[2];
};

static double rnd(sim_t *s) {
  s->rng ^= s->rng << 13;
  s->rng ^= s->rng >> 7;
  s->rng ^= s->rng << 17;
  return (double)(s->rng >> 11) / (double)(1ull << 53);
}

static uint64_t node_now(void *user) { return ((node_t *)user)->sim->now_ns; }

static int node_send(void *user, const rudp_out_packet *packets, size_t count) {
  node_t *n = user;
  sim_t *s = n->sim;
  link_t *l = &s->links[n->side];
  for (size_t i = 0; i < count; ++i) {
    if (rnd(s) < l->loss) continue;
    uint64_t depart = s->now_ns;
    if (l->rate_bytes_per_s != 0) {
      const uint64_t start = l->busy_until_ns > s->now_ns ? l->busy_until_ns : s->now_ns;
      const uint64_t queued = (start - s->now_ns) * l->rate_bytes_per_s / 1000000000ull;
      if (queued + packets[i].len > l->queue_limit_bytes) continue;
      depart = start + packets[i].len * 1000000000ull / l->rate_bytes_per_s;
      l->busy_until_ns = depart;
    }
    CHECK(l->count < MAX_PKTS);
    pkt *p = &l->q[l->count++];
    double delay_ms = 50.0;
    if (l->jitter_ms > 0) {
      const double u1 = rnd(s) + 1e-12, u2 = rnd(s);
      delay_ms += l->jitter_ms * sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
      if (delay_ms < 0) delay_ms = 0;
    }
    p->at_ns = depart + (uint64_t)(delay_ms * (double)MS);
    p->len = packets[i].len;
    memcpy(p->data, packets[i].data, packets[i].len);
  }
  return (int)count;
}

static int node_recv(void *user, rudp_in_packet *packets, size_t max_count) {
  node_t *n = user;
  sim_t *s = n->sim;
  link_t *l = &s->links[1 - n->side];
  size_t got = 0;
  for (size_t i = 0; got < max_count && i < l->count;) {
    pkt *p = &l->q[i];
    if (p->at_ns > s->now_ns) {
      i++;
      continue;
    }
    memcpy(packets[got].data, p->data, p->len);
    packets[got].len = p->len;
    packets[got].addr.len = 1;
    packets[got].addr.data[0] = (uint8_t)(1 - n->side);
    got++;
    *p = l->q[--l->count];
  }
  return (int)got;
}

static void sim_init(sim_t *s, double loss) {
  memset(s, 0, sizeof(*s));
  s->rng = 88172645463325252ull;
  for (int i = 0; i < 2; ++i) {
    s->links[i].q = calloc(MAX_PKTS, sizeof(pkt));
    CHECK(s->links[i].q != NULL);
    s->links[i].loss = loss;
    node_t *n = &s->nodes[i];
    n->sim = s;
    n->side = i;
    const rudp_endpoint_config cfg = {
        .socket = {.user = n, .send_batch = node_send, .recv_batch = node_recv, .now_ns = node_now},
        .mtu = MTU,
        .max_conns = 1,
        .max_flows = 1,
        .max_channels = 2,
    };
    CHECK(rudp_endpoint_create(&n->ep, &cfg) == 0);
  }
}

static void sim_free(sim_t *s) {
  for (int i = 0; i < 2; ++i) {
    rudp_endpoint_destroy(s->nodes[i].ep);
    free(s->links[i].q);
  }
}

static rudp_conn *sim_connect(sim_t *s) {
  rudp_addr server = {.len = 1};
  server.data[0] = 0;
  rudp_conn *c = NULL;
  CHECK(rudp_endpoint_connect(s->nodes[1].ep, &server, 1, &c) == 0);
  return c;
}

// 1 ms 進めて、両端で受信と送信を回す。受信したものは on_recv に渡す。
static void sim_step(sim_t *s, void (*on_recv)(void *ctx, int side, const uint8_t *data, size_t len, rudp_reliability rel), void *ctx) {
  s->now_ns += MS;
  for (int i = 0; i < 2; ++i) {
    rudp_endpoint *ep = s->nodes[i].ep;
    rudp_endpoint_poll(ep, s->now_ns);
    uint8_t buf[MTU];
    size_t len = 0;
    rudp_recv_info info;
    while (rudp_recv(ep, buf, sizeof(buf), &len, &info) == 1) {
      if (on_recv != NULL) on_recv(ctx, i, buf, len, info.reliability);
    }
    rudp_endpoint_flush(ep, s->now_ns);
  }
}

static rudp_send_result send_msg(rudp_conn *c, uint64_t now_ns, size_t len, int md) {
  uint8_t buf[MTU] = {0};
  memcpy(buf, &now_ns, sizeof(now_ns));
  const rudp_send_opts opts = {
      .channel_id = md ? CHANNEL_MD : CHANNEL_LT,
      .reliability = md ? RUDP_RELIABLE_ORDERED : RUDP_UNRELIABLE_SEQUENCED,
  };
  return rudp_send(c, buf, len, &opts);
}

typedef struct {
  uint64_t now_ns;
  uint64_t measure_from_ns;
  uint64_t lt_recv;
  uint64_t md_recv;
  uint64_t md_late;  // 400 ms を超えて届いた MD
  uint64_t lt_late;  // 500 ms を超えて古い LT
} stats_t;

static void count_recv(void *ctx, int side, const uint8_t *data, size_t len, rudp_reliability rel) {
  stats_t *st = ctx;
  if (side != 1 || len < sizeof(uint64_t)) return;
  uint64_t sent;
  memcpy(&sent, data, sizeof(sent));
  if (sent < st->measure_from_ns) return;
  const uint64_t age = st->now_ns - sent;
  if (rel == RUDP_RELIABLE_ORDERED) {
    st->md_recv++;
    if (age > 400 * MS) st->md_late++;
  } else {
    st->lt_recv++;
    if (age > 500 * MS) st->lt_late++;
  }
}

// server から client へ、LT 1000 B を 20 Hz、MD 64 B を 10 Hz で送る(劣悪ベンチの下りの 1 接続分)。
static stats_t run_downlink(sim_t *s, uint64_t seconds, uint64_t measure_from_s) {
  sim_connect(s);
  // client の接続を server に知らせるため、client から一度送る
  rudp_conn *cc = rudp_endpoint_find_conn(s->nodes[1].ep, 1);
  CHECK(send_msg(cc, 0, 64, 1) == RUDP_SEND_OK || send_msg(cc, 0, 64, 1) == RUDP_SEND_QUEUED);
  stats_t st = {.measure_from_ns = measure_from_s * 1000 * MS};
  rudp_conn *sc = NULL;
  for (uint64_t ms = 1; ms <= seconds * 1000; ++ms) {
    st.now_ns = s->now_ns + MS;
    sim_step(s, count_recv, &st);
    if (sc == NULL) {
      sc = rudp_endpoint_find_conn(s->nodes[0].ep, 1);
      continue;
    }
    if (ms % 50 == 0) send_msg(sc, s->now_ns, 1000, 0);
    if (ms % 100 == 0) send_msg(sc, s->now_ns, 64, 1);
  }
  CHECK(sc != NULL);
  return st;
}

// 混雑によらないロスでは送る量を絞らない。
static void test_random_loss_does_not_throttle(void) {
  sim_t s;
  sim_init(&s, 0.10);
  const stats_t st = run_downlink(&s, 20, 10);
  // 10 秒の LT は 200 件。片道 10% のロスなら約 180 件届く
  fprintf(stderr, "random loss: lt_recv=%llu lt_late=%llu\n", (unsigned long long)st.lt_recv, (unsigned long long)st.lt_late);
  CHECK(st.lt_recv >= 170);
  CHECK(st.lt_late == 0);
  sim_free(&s);
}

// ゆらぎを混雑と取り違えて送る量を絞らない。
static void test_jitter_does_not_throttle(void) {
  sim_t s;
  sim_init(&s, 0.02);
  s.links[0].jitter_ms = 30;
  s.links[1].jitter_ms = 30;
  run_downlink(&s, 20, 10);
  rudp_status status;
  rudp_get_status(rudp_endpoint_find_conn(s.nodes[0].ep, 1), &status);
  fprintf(stderr, "jitter: safe_bps=%u\n", status.safe_bps);
  CHECK(status.safe_bps == UINT32_MAX);
  sim_free(&s);
}

// 混雑では送る量を減らし、待ち行列を溜めない。
static void test_congestion_keeps_delay_low(void) {
  sim_t s;
  sim_init(&s, 0.0);
  // 下りを 1 Mbit/s の 8 分の 1(125 kbit/s)に絞り、待ち行列を約 1 秒分にする
  s.links[0].rate_bytes_per_s = 125000 / 8;
  s.links[0].queue_limit_bytes = 125000 / 8;
  const stats_t st = run_downlink(&s, 40, 20);
  fprintf(stderr, "congestion: md_recv=%llu md_late=%llu lt_recv=%llu lt_late=%llu\n", (unsigned long long)st.md_recv,
          (unsigned long long)st.md_late, (unsigned long long)st.lt_recv, (unsigned long long)st.lt_late);
  // 20 秒の MD は 200 件。届き切り、99% は 400 ms 以内
  CHECK(st.md_recv >= 195);
  CHECK(st.md_late <= 2);
  CHECK(st.lt_late <= st.lt_recv / 100);
  sim_free(&s);
}

// 送れない間に溜まった sequenced は、最新の 1 件だけを残す。
static void test_sequenced_keeps_only_latest(void) {
  sim_t s;
  sim_init(&s, 0.0);
  rudp_conn *c = sim_connect(&s);
  for (int i = 0; i < 10; ++i) {
    CHECK(send_msg(c, s.now_ns, 1000, 0) != RUDP_SEND_QUEUE_FULL);
  }
  rudp_status status;
  rudp_get_status(c, &status);
  CHECK(status.send_queue_bytes == 1000);
  sim_free(&s);
}

int main(int argc, char **argv) {
  static const struct {
    const char *name;
    void (*fn)(void);
  } tests[] = {
      {"sequenced_keeps_only_latest", test_sequenced_keeps_only_latest},
      {"random_loss_does_not_throttle", test_random_loss_does_not_throttle},
      {"jitter_does_not_throttle", test_jitter_does_not_throttle},
      {"congestion_keeps_delay_low", test_congestion_keeps_delay_low},
  };
  for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
    if (argc < 2 || strcmp(argv[1], tests[i].name) == 0) tests[i].fn();
  }
  return 0;
}
