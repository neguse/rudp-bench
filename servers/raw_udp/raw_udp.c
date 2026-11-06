// 再送を持たない素の UDP。回線再現と計測器を確かめる物差しで、候補ではない。
// LT も MD も同じ datagram で送るため、MD は unsupported と開示する。
#include "bk_sut.h"

#include "../udp_conns.h"

typedef struct {
  bk_sut *sut;
  udp_conns u;
  bool announced;  // client: on_connect を出したか
} raw_t;

static void raw_close(void *t) {
  raw_t *r = t;
  udp_conns_close(&r->u);
  free(r);
}

static raw_t *raw_new(bk_sut *sut) {
  raw_t *r = calloc(1, sizeof(*r));
  if (r != NULL) {
    r->sut = sut;
  }
  return r;
}

static void *raw_listen(bk_sut *sut, uint16_t port, int max_conns) {
  raw_t *r = raw_new(sut);
  if (r == NULL || udp_conns_listen(&r->u, port, max_conns) != 0) {
    free(r);
    return NULL;
  }
  return r;
}

static void *raw_connect(bk_sut *sut, const char *host, uint16_t port,
                         int conns) {
  raw_t *r = raw_new(sut);
  if (r == NULL || udp_conns_connect(&r->u, host, port, conns) != 0) {
    free(r);
    return NULL;
  }
  return r;
}

static int raw_send(void *t, int conn, bool must_deliver, const void *data,
                    size_t len) {
  (void)must_deliver;
  raw_t *r = t;
  return udp_conns_send(&r->u, conn, data, len);
}

static void on_datagram(void *ctx, int conn, bool is_new,
                        const unsigned char *data, size_t len) {
  raw_t *r = ctx;
  if (is_new) {
    bk_sut_on_connect(r->sut, conn);
  }
  bk_sut_on_receive(r->sut, conn, data, len);
}

static int raw_service(void *t, uint64_t timeout_ns) {
  raw_t *r = t;
  // UDP に接続の手順はないので、client は最初の service で全接続をつながったことにする
  if (!r->u.server && !r->announced) {
    r->announced = true;
    for (int i = 0; i < r->u.n; ++i) {
      bk_sut_on_connect(r->sut, i);
    }
  }
  return udp_conns_poll(&r->u, (int)(timeout_ns / 1000000u), on_datagram, r);
}

static const bk_transport transport = {
    .name = "raw_udp",
    .describe =
        "\"class_mapping\":{"
        "\"loss_tolerant\":{\"primitive\":\"udp datagram\","
        "\"delivery\":\"best_effort\",\"ordering\":\"unordered\","
        "\"realization\":\"native\"},"
        "\"must_deliver\":{\"primitive\":\"udp datagram\","
        "\"delivery\":\"best_effort\",\"ordering\":\"unordered\","
        "\"realization\":\"unsupported\"}},"
        "\"coalescing\":\"none\",\"cc_algo\":\"none\","
        "\"thread_model\":\"single\",\"encryption\":false,\"tuning\":[]",
    .max_payload = 1400,
    .listen = raw_listen,
    .connect = raw_connect,
    .send = raw_send,
    .service = raw_service,
    .close = raw_close,
};

int main(int argc, char **argv) { return bk_sut_main(argc, argv, &transport); }
