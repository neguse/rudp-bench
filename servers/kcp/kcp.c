// KCP の層。KCP は reliable な ARQ だけを持ち、socket も unreliable な送り方も
// 持たない(README.en.md)。同じ UDP socket の上で LT は素の datagram、MD は KCP に
// 載せ、先頭 1 byte で分ける。KCP の設定は ikcp_create の既定のまま。
#include "bk_sut.h"

#include "../udp_conns.h"
#include "benchkit.h"
#include "ikcp.h"

#include <string.h>

#define PREFIX_LT 0
#define PREFIX_MD 1
// 接続は送信元アドレスで分けるので、conv は全接続で同じでよい
#define CONV 1

typedef struct kcp_t kcp_t;

typedef struct {
  kcp_t *k;
  int conn;
  ikcpcb *kcp;
} kcp_conn;

struct kcp_t {
  bk_sut *sut;
  udp_conns u;
  kcp_conn *conns;
  bool announced;  // client: on_connect を出したか
  unsigned char out[UDP_CONNS_MAX_DATAGRAM];
  unsigned char in[UDP_CONNS_MAX_DATAGRAM];
};

static IUINT32 now_ms(void) { return (IUINT32)(bk_now_ns() / 1000000u); }

static int output(const char *data, int len, ikcpcb *kcp, void *user) {
  (void)kcp;
  kcp_conn *c = user;
  c->k->out[0] = PREFIX_MD;
  memcpy(c->k->out + 1, data, (size_t)len);
  udp_conns_send(&c->k->u, c->conn, c->k->out, (size_t)len + 1u);
  return 0;
}

static int open_conn(kcp_t *k, int conn) {
  kcp_conn *c = &k->conns[conn];
  c->k = k;
  c->conn = conn;
  c->kcp = ikcp_create(CONV, c);
  if (c->kcp == NULL) {
    return -1;
  }
  ikcp_setoutput(c->kcp, output);
  return 0;
}

static void kcp_close(void *t) {
  kcp_t *k = t;
  for (int i = 0; k->conns != NULL && i < k->u.n; ++i) {
    if (k->conns[i].kcp != NULL) {
      ikcp_release(k->conns[i].kcp);
    }
  }
  free(k->conns);
  udp_conns_close(&k->u);
  free(k);
}

static void *kcp_listen(bk_sut *sut, uint16_t port, int max_conns) {
  kcp_t *k = calloc(1, sizeof(*k));
  if (k == NULL) {
    return NULL;
  }
  k->sut = sut;
  if (udp_conns_listen(&k->u, port, max_conns) != 0) {
    free(k);
    return NULL;
  }
  k->conns = calloc((size_t)max_conns, sizeof(*k->conns));
  if (k->conns == NULL) {
    kcp_close(k);
    return NULL;
  }
  return k;
}

static void *kcp_connect(bk_sut *sut, const char *host, uint16_t port,
                         int conns) {
  kcp_t *k = calloc(1, sizeof(*k));
  if (k == NULL) {
    return NULL;
  }
  k->sut = sut;
  if (udp_conns_connect(&k->u, host, port, conns) != 0) {
    free(k);
    return NULL;
  }
  k->conns = calloc((size_t)conns, sizeof(*k->conns));
  bool ok = k->conns != NULL;
  for (int i = 0; ok && i < conns; ++i) {
    ok = open_conn(k, i) == 0;
  }
  if (!ok) {
    kcp_close(k);
    return NULL;
  }
  return k;
}

static int kcp_send(void *t, int conn, bool must_deliver, const void *data,
                    size_t len) {
  kcp_t *k = t;
  if (must_deliver) {
    return ikcp_send(k->conns[conn].kcp, data, (int)len) < 0 ? -1 : 0;
  }
  if (len + 1u > sizeof(k->out)) {
    return -1;
  }
  k->out[0] = PREFIX_LT;
  memcpy(k->out + 1, data, len);
  return udp_conns_send(&k->u, conn, k->out, len + 1u);
}

static void on_datagram(void *ctx, int conn, bool is_new,
                        const unsigned char *data, size_t len) {
  kcp_t *k = ctx;
  if (is_new) {
    if (open_conn(k, conn) != 0) {
      return;
    }
    bk_sut_on_connect(k->sut, conn);
  }
  if (len >= 1 && data[0] == PREFIX_LT) {
    bk_sut_on_receive(k->sut, conn, data + 1, len - 1u);
    return;
  }
  if (len < 1 || data[0] != PREFIX_MD) {
    bk_sut_on_receive(k->sut, conn, data, 0);
    return;
  }
  ikcpcb *kcp = k->conns[conn].kcp;
  ikcp_input(kcp, (const char *)data + 1, (long)len - 1);
  int n;
  while ((n = ikcp_recv(kcp, (char *)k->in, (int)sizeof(k->in))) > 0) {
    bk_sut_on_receive(k->sut, conn, k->in, (size_t)n);
  }
}

static int kcp_service(void *t, uint64_t timeout_ns) {
  kcp_t *k = t;
  const int n = k->u.server ? k->u.known : k->u.n;
  // KCP に接続の手順はないので、client は最初の service で全接続をつながったことにする
  if (!k->u.server && !k->announced) {
    k->announced = true;
    for (int i = 0; i < n; ++i) {
      bk_sut_on_connect(k->sut, i);
    }
  }
  // ikcp_check が示す次の更新時刻より長くは待たない(README.en.md の使い方)
  const IUINT32 now = now_ms();
  IUINT32 wait = (IUINT32)(timeout_ns / 1000000u);
  for (int i = 0; i < n; ++i) {
    const IUINT32 due = ikcp_check(k->conns[i].kcp, now) - now;
    if (due < wait) {
      wait = due;
    }
  }
  if (udp_conns_poll(&k->u, (int)wait, on_datagram, k) != 0) {
    return -1;
  }
  const IUINT32 after = now_ms();
  for (int i = 0; i < (k->u.server ? k->u.known : k->u.n); ++i) {
    ikcp_update(k->conns[i].kcp, after);
  }
  return 0;
}

static int64_t kcp_send_queue(void *t, int conn) {
  const kcp_t *k = t;
  return ikcp_waitsnd(k->conns[conn].kcp);
}

static const bk_transport transport = {
    .name = "kcp",
    .describe =
        "\"class_mapping\":{"
        "\"loss_tolerant\":{\"primitive\":\"udp datagram beside kcp\","
        "\"delivery\":\"best_effort\",\"ordering\":\"unordered\","
        "\"realization\":\"native\"},"
        "\"must_deliver\":{\"primitive\":\"kcp\","
        "\"delivery\":\"reliable\",\"ordering\":\"ordered\","
        "\"realization\":\"native\"}},"
        "\"coalescing\":\"kcp segment\",\"cc_algo\":\"kcp default (cwnd on)\","
        "\"thread_model\":\"single\",\"encryption\":false,\"tuning\":[]",
    .max_payload = 1400,
    .listen = kcp_listen,
    .connect = kcp_connect,
    .send = kcp_send,
    .service = kcp_service,
    .send_queue = kcp_send_queue,
    .send_queue_unit = "segments",
    .close = kcp_close,
};

int main(int argc, char **argv) { return bk_sut_main(argc, argv, &transport); }
