// quiche の層。msquic と同じく、LT は QUIC datagram(include/quiche.h の quiche_conn_dgram_send)、MD は接続ごとに 1 本の双方向 stream(include/quiche.h の quiche_conn_stream_send)に長さ 4 byte をつけて載せる。
// stream は client が開く id 0 を使う。
//
// quiche は socket と timer を持たないので、udp_conns の上で quiche_conn_recv、quiche_conn_send、quiche_conn_on_timeout を回す(quiche/src/lib.rs 先頭の文書の使い方)。
// quiche_conn_send が返す送信時刻(send_info.at)は使わず、examples/server.c と同じくすぐ送る。
//
// quiche_conn_stream_send は輻輳制御とフロー制御で送れる分しか受け取らないので、残りはこの層で溜めて、送れるようになったら渡す。
//
// 既定から変えるのは、既定のままでは stream と datagram を使えない値と、既定では無効な idle timeout だけにする。
// 値は quiche の apps(apps/src/args.rs と apps/src/bin/quiche-server.rs)に合わせる。
//
// QUICHE_DROP_OLD_DGRAM を定義すると、LT の datagram を送信待ちに足す前に、まだ送っていない datagram を捨てる(include/quiche.h の quiche_conn_dgram_purge_outgoing)。
// LT は最新の値が届けばよいので、送れずに残った古い値は捨ててよい。
#include "bk_sut.h"

#include "../udp_conns.h"
#include "quiche.h"

#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>

#define STREAM_ID 0
#ifdef QUICHE_DROP_OLD_DGRAM
#define NAME "quiche-drop-old-dgram"
#define LT_TUNING                                                   \
  ",{\"knob\":\"quiche_conn_dgram_purge_outgoing\",\"value\":\"LT\"," \
  "\"reason\":\"LT only needs the latest value\","                   \
  "\"upstream_ref\":\"include/quiche.h\"}"
#else
#define NAME "quiche"
#define LT_TUNING ""
#endif
#define CID_LEN 16

static const uint8_t kAlpn[] = "\x0arudp-bench";

typedef struct {
  quiche_conn *q;
  bool connected;  // on_connect を出したか
  bool closed;     // on_disconnect を出したか
  uint8_t *tx;     // stream に渡せていない MD の bytes
  size_t tx_len, tx_cap;
  uint8_t rx[16384];  // stream の受信途中の bytes
  size_t rx_len;
} conn;

typedef struct {
  bk_sut *sut;
  quiche_config *config;
  udp_conns u;
  conn *conns;
  struct sockaddr_in local;  // server の socket の address
  struct sockaddr_in peer;   // client: server の address
  uint8_t in[UDP_CONNS_MAX_DATAGRAM];
  uint8_t out[UDP_CONNS_MAX_DATAGRAM];
} quiche_t;

static int n_conns(const quiche_t *t) { return t->u.server ? t->u.known : t->u.n; }

static const struct sockaddr *peer_of(const quiche_t *t, int i) {
  return (const struct sockaddr *)(t->u.server ? &t->u.peers[i] : &t->peer);
}

static int local_of(const quiche_t *t, int i, struct sockaddr_in *out) {
  if (t->u.server) {
    *out = t->local;
    return 0;
  }
  socklen_t len = sizeof(*out);
  return getsockname(t->u.pfds[i].fd, (struct sockaddr *)out, &len);
}

static void push_md(conn *c) {
  if (c->tx_len == 0) {
    return;
  }
  const ssize_t n = quiche_conn_stream_send(c->q, STREAM_ID, c->tx, c->tx_len,
                                            false, NULL);
  if (n > 0) {
    memmove(c->tx, c->tx + n, c->tx_len - (size_t)n);
    c->tx_len -= (size_t)n;
  }
}

static void egress(quiche_t *t, int i) {
  conn *c = &t->conns[i];
  push_md(c);
  quiche_send_info info;
  ssize_t n;
  while ((n = quiche_conn_send(c->q, t->out, sizeof(t->out), &info)) > 0) {
    udp_conns_send(&t->u, i, t->out, (size_t)n);
  }
}

static void ingress(quiche_t *t, int i) {
  conn *c = &t->conns[i];
  if (!c->connected && quiche_conn_is_established(c->q)) {
    c->connected = true;
    bk_sut_on_connect(t->sut, i);
  }
  ssize_t n;
  while ((n = quiche_conn_dgram_recv(c->q, t->in, sizeof(t->in))) >= 0) {
    bk_sut_on_receive(t->sut, i, t->in, (size_t)n);
  }
  bool fin = false;
  while ((n = quiche_conn_stream_recv(c->q, STREAM_ID, c->rx + c->rx_len,
                                      sizeof(c->rx) - c->rx_len, &fin,
                                      NULL)) > 0) {
    c->rx_len += (size_t)n;
    size_t off = 0;
    for (;;) {
      uint32_t len;
      if (c->rx_len - off < sizeof(len)) {
        break;
      }
      memcpy(&len, c->rx + off, sizeof(len));
      len = ntohl(len);
      if (c->rx_len - off < sizeof(len) + len) {
        break;
      }
      bk_sut_on_receive(t->sut, i, c->rx + off + sizeof(len), len);
      off += sizeof(len) + len;
    }
    memmove(c->rx, c->rx + off, c->rx_len - off);
    c->rx_len -= off;
  }
}

static void check_closed(quiche_t *t, int i) {
  conn *c = &t->conns[i];
  if (!c->closed && quiche_conn_is_closed(c->q)) {
    c->closed = true;
    fprintf(stderr, "quiche: conn %d closed timed_out=%d\n", i,
            quiche_conn_is_timed_out(c->q));
    bk_sut_on_disconnect(t->sut, i);
  }
}

static void quiche_close_t(void *p) {
  quiche_t *t = p;
  for (int i = 0; t->conns != NULL && i < t->u.n; ++i) {
    if (t->conns[i].q != NULL) {
      quiche_conn_free(t->conns[i].q);
    }
    free(t->conns[i].tx);
  }
  free(t->conns);
  udp_conns_close(&t->u);
  if (t->config != NULL) {
    quiche_config_free(t->config);
  }
  free(t);
}

static quiche_t *create(bk_sut *sut, bool server) {
  quiche_t *t = calloc(1, sizeof(*t));
  if (t == NULL) {
    return NULL;
  }
  t->sut = sut;
  quiche_config *c = quiche_config_new(QUICHE_PROTOCOL_VERSION);
  t->config = c;
  if (c == NULL ||
      quiche_config_set_application_protos(c, kAlpn, sizeof(kAlpn) - 1) != 0) {
    quiche_close_t(t);
    return NULL;
  }
  // 既定の 0 では stream を開けず、datagram も溜められないので送れない(quiche/src/lib.rs の Config::new と quiche/src/transport_params.rs の TransportParams::default)。
  quiche_config_set_initial_max_data(c, 10000000);
  quiche_config_set_initial_max_stream_data_bidi_local(c, 1000000);
  quiche_config_set_initial_max_stream_data_bidi_remote(c, 1000000);
  quiche_config_set_initial_max_streams_bidi(c, 100);
  quiche_config_enable_dgram(c, true, 1000, 1000);
  // 既定は idle timeout なしで、相手が消えても接続が閉じない
  quiche_config_set_max_idle_timeout(c, 30000);
  if (server) {
    // quiche の examples の自己署名の証明書(client は検証しない)
    if (quiche_config_load_cert_chain_from_pem_file(
            c, QUICHE_EXAMPLES_DIR "/cert.crt") != 0 ||
        quiche_config_load_priv_key_from_pem_file(
            c, QUICHE_EXAMPLES_DIR "/cert.key") != 0) {
      quiche_close_t(t);
      return NULL;
    }
  } else {
    quiche_config_verify_peer(c, false);
  }
  return t;
}

static void *quiche_listen(bk_sut *sut, uint16_t port, int max_conns) {
  quiche_t *t = create(sut, true);
  if (t == NULL) {
    return NULL;
  }
  socklen_t len = sizeof(t->local);
  if (udp_conns_listen(&t->u, port, max_conns) != 0 ||
      getsockname(t->u.pfds[0].fd, (struct sockaddr *)&t->local, &len) != 0 ||
      (t->conns = calloc((size_t)max_conns, sizeof(*t->conns))) == NULL) {
    quiche_close_t(t);
    return NULL;
  }
  return t;
}

static void *quiche_connect_t(bk_sut *sut, const char *host, uint16_t port,
                              int conns) {
  quiche_t *t = create(sut, false);
  if (t == NULL) {
    return NULL;
  }
  t->peer = (struct sockaddr_in){.sin_family = AF_INET, .sin_port = htons(port)};
  bool ok = inet_pton(AF_INET, host, &t->peer.sin_addr) == 1 &&
            udp_conns_connect(&t->u, host, port, conns) == 0 &&
            (t->conns = calloc((size_t)conns, sizeof(*t->conns))) != NULL;
  for (int i = 0; ok && i < conns; ++i) {
    uint8_t scid[CID_LEN];
    struct sockaddr_in local;
    ok = getrandom(scid, sizeof(scid), 0) == sizeof(scid) &&
         local_of(t, i, &local) == 0 &&
         (t->conns[i].q = quiche_connect(
              NULL, scid, sizeof(scid), (const struct sockaddr *)&local,
              sizeof(local), peer_of(t, i), sizeof(t->peer), t->config)) != NULL;
    if (ok) {
      egress(t, i);
    }
  }
  if (!ok) {
    quiche_close_t(t);
    return NULL;
  }
  return t;
}

#ifdef QUICHE_DROP_OLD_DGRAM
static bool purge_all(uint8_t *data, size_t len) {
  (void)data;
  (void)len;
  return true;
}
#endif

static int quiche_send_t(void *p, int i, bool must_deliver, const void *data,
                         size_t len) {
  quiche_t *t = p;
  conn *c = &t->conns[i];
  if (c->q == NULL || c->closed) {
    return -1;
  }
  if (!must_deliver) {
#ifdef QUICHE_DROP_OLD_DGRAM
    quiche_conn_dgram_purge_outgoing(c->q, purge_all);
#endif
    return quiche_conn_dgram_send(c->q, data, len) < 0 ? -1 : 0;
  }
  const uint32_t nlen = htonl((uint32_t)len);
  if (c->tx_len + sizeof(nlen) + len > c->tx_cap) {
    const size_t cap = (c->tx_cap + sizeof(nlen) + len) * 2;
    uint8_t *tx = realloc(c->tx, cap);
    if (tx == NULL) {
      return -1;
    }
    c->tx = tx;
    c->tx_cap = cap;
  }
  memcpy(c->tx + c->tx_len, &nlen, sizeof(nlen));
  memcpy(c->tx + c->tx_len + sizeof(nlen), data, len);
  c->tx_len += sizeof(nlen) + len;
  push_md(c);
  return 0;
}

static void quiche_flush(void *p) {
  quiche_t *t = p;
  for (int i = 0; i < n_conns(t); ++i) {
    egress(t, i);
  }
}

static void on_datagram(void *ctx, int i, bool is_new,
                        const unsigned char *data, size_t len) {
  quiche_t *t = ctx;
  conn *c = &t->conns[i];
  if (is_new) {
    uint8_t scid[CID_LEN];
    if (getrandom(scid, sizeof(scid), 0) != sizeof(scid)) {
      return;
    }
    c->q = quiche_accept(scid, sizeof(scid), NULL, 0,
                         (const struct sockaddr *)&t->local, sizeof(t->local),
                         peer_of(t, i), sizeof(t->u.peers[i]), t->config);
  }
  if (c->q == NULL || len > sizeof(t->in)) {
    return;
  }
  // quiche_conn_recv は buf をその場で復号するので、書き換えてよい領域に写す
  memcpy(t->in, data, len);
  struct sockaddr_in local;
  if (local_of(t, i, &local) != 0) {
    return;
  }
  const quiche_recv_info info = {
      .from = (struct sockaddr *)peer_of(t, i),
      .from_len = sizeof(struct sockaddr_in),
      .to = (struct sockaddr *)&local,
      .to_len = sizeof(local),
  };
  quiche_conn_recv(c->q, t->in, len, &info);
  ingress(t, i);
}

static int quiche_service(void *p, uint64_t timeout_ns) {
  quiche_t *t = p;
  uint64_t wait = timeout_ns;
  for (int i = 0; i < n_conns(t); ++i) {
    if (t->conns[i].q != NULL) {
      const uint64_t due = quiche_conn_timeout_as_nanos(t->conns[i].q);
      wait = due < wait ? due : wait;
    }
  }
  // poll は ms 単位なので、期限より前に起きて空回りしないよう切り上げる
  if (udp_conns_poll(&t->u, (int)((wait + 999999u) / 1000000u), on_datagram, t) !=
      0) {
    return -1;
  }
  for (int i = 0; i < n_conns(t); ++i) {
    if (t->conns[i].q == NULL) {
      continue;
    }
    if (quiche_conn_timeout_as_nanos(t->conns[i].q) == 0) {
      quiche_conn_on_timeout(t->conns[i].q);
    }
    egress(t, i);
    check_closed(t, i);
  }
  return 0;
}

// LT の datagram の送信待ち。stream の送信待ちを返す API はない
static int64_t quiche_send_queue(void *p, int i) {
  const quiche_t *t = p;
  const conn *c = &t->conns[i];
  if (c->q == NULL) {
    return 0;
  }
  return quiche_conn_dgram_send_queue_byte_size(c->q);
}

static const bk_transport transport = {
    .name = NAME,
    .describe =
        "\"class_mapping\":{"
        "\"loss_tolerant\":{\"primitive\":\"quic datagram\","
        "\"delivery\":\"best_effort\",\"ordering\":\"unordered\","
        "\"realization\":\"native\"},"
        "\"must_deliver\":{\"primitive\":\"quic stream\","
        "\"delivery\":\"reliable\",\"ordering\":\"ordered\","
        "\"realization\":\"native\"}},"
        "\"coalescing\":\"none\",\"cc_algo\":\"cubic (default)\","
        "\"thread_model\":\"single\",\"encryption\":true,"
        "\"tuning\":["
        "{\"knob\":\"initial_max_data\",\"value\":\"10000000\","
        "\"reason\":\"default 0 blocks stream data\","
        "\"upstream_ref\":\"apps/src/args.rs\"},"
        "{\"knob\":\"initial_max_stream_data_bidi_local\",\"value\":\"1000000\","
        "\"reason\":\"default 0 blocks stream data\","
        "\"upstream_ref\":\"apps/src/args.rs\"},"
        "{\"knob\":\"initial_max_stream_data_bidi_remote\",\"value\":\"1000000\","
        "\"reason\":\"default 0 blocks stream data\","
        "\"upstream_ref\":\"apps/src/args.rs\"},"
        "{\"knob\":\"initial_max_streams_bidi\",\"value\":\"100\","
        "\"reason\":\"default 0 forbids opening a stream\","
        "\"upstream_ref\":\"apps/src/args.rs\"},"
        "{\"knob\":\"enable_dgram\",\"value\":\"true, 1000, 1000\","
        "\"reason\":\"default off; default queue length 0 accepts no datagram\","
        "\"upstream_ref\":\"apps/src/bin/quiche-server.rs\"},"
        "{\"knob\":\"max_idle_timeout\",\"value\":\"30000 ms\","
        "\"reason\":\"default none never closes a dead connection\","
        "\"upstream_ref\":\"apps/src/args.rs\"}" LT_TUNING "]",
    .max_payload = 1000,
    .listen = quiche_listen,
    .connect = quiche_connect_t,
    .send = quiche_send_t,
    .flush = quiche_flush,
    .service = quiche_service,
    .send_queue = quiche_send_queue,
    .send_queue_unit = "bytes",
    .close = quiche_close_t,
};

int main(int argc, char **argv) { return bk_sut_main(argc, argv, &transport); }
