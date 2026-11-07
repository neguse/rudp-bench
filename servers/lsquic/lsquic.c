// lsquic の層。msquic と同じく、LT は QUIC datagram(RFC 9221)、MD は接続ごとに 1 本の双方向 stream に長さ 4 byte をつけて載せる。
// datagram は既定で無効なので es_datagrams を有効にする(docs/apiref.rst の Datagrams)。
//
// lsquic は送るものを溜めない。
// datagram は lsquic が packet に空きのあるときに on_dg_write で 1 つずつ取りに来る(docs/apiref.rst の on_dg_write)。
// stream は lsquic_stream_write が受け取れた分だけを返す(docs/apiref.rst の lsquic_stream_write)。
// そのため送るものは接続ごとの列に溜め、msquic の既定と同じく上限なく持つ。
// stream は書いた bytes を 1 packet 分になるまで出さないので、書くたびに lsquic_stream_flush する。
//
// socket は持たないので udp_conns.h の socket を lsquic_engine_packet_in と packets_out でつなぐ。
// callback はすべて service() の中の lsquic_engine_process_conns と lsquic_engine_packet_in から呼ばれる。
#include "bk_sut.h"

#include "../udp_conns.h"

#include <lsquic.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <string.h>

#define MAX_PAYLOAD 1000

// TLS の ALPN の形(先頭 1 byte が長さ)
static const unsigned char kAlpn[] = "\x0a" "rudp-bench";

typedef struct msg {
  struct msg *next;
  size_t len;
  size_t off;  // MD: stream に書き終えた bytes
  unsigned char data[];
} msg;

typedef struct {
  msg *head;
  msg *tail;
} queue;

typedef struct lsq_t lsq_t;

typedef struct {
  lsq_t *q;
  int index;
  lsquic_conn_t *conn;
  lsquic_stream_t *stream;
  queue lt;
  queue md;
  size_t rx_len;  // stream の受信途中の bytes
  unsigned char rx[sizeof(uint32_t) + MAX_PAYLOAD];
} conn_t;

struct lsq_t {
  bk_sut *sut;
  int n;
  conn_t *conns;
  udp_conns u;
  struct sockaddr_in *local;  // socket ごとの自分のアドレス
  struct sockaddr_in server;  // client: 接続先
  SSL_CTX *ssl;               // server
  lsquic_engine_t *engine;
  bool closing;
};

static void push(queue *qu, msg *m) {
  if (qu->tail != NULL) {
    qu->tail->next = m;
  } else {
    qu->head = m;
  }
  qu->tail = m;
}

static msg *pop(queue *qu) {
  msg *m = qu->head;
  qu->head = m->next;
  if (qu->head == NULL) {
    qu->tail = NULL;
  }
  return m;
}

static void clear(queue *qu) {
  while (qu->head != NULL) {
    free(pop(qu));
  }
}

static conn_t *conn_of(lsquic_conn_t *conn) {
  return (conn_t *)lsquic_conn_get_ctx(conn);
}

static lsquic_conn_ctx_t *on_new_conn(void *ctx, lsquic_conn_t *conn) {
  const lsq_t *q = ctx;
  conn_t *c = lsquic_conn_get_peer_ctx(conn, NULL);
  c->conn = conn;
  // server はハンドシェイクを終えてからここに来る。
  // client は stream ができてから on_new_stream で知らせる。
  if (q->u.server) {
    bk_sut_on_connect(q->sut, c->index);
  }
  return (lsquic_conn_ctx_t *)c;
}

static void on_hsk_done(lsquic_conn_t *conn, enum lsquic_hsk_status s) {
  if (s == LSQ_HSK_OK || s == LSQ_HSK_RESUMED_OK) {
    lsquic_conn_make_stream(conn);
  }
}

static void on_conn_closed(lsquic_conn_t *conn) {
  conn_t *c = conn_of(conn);
  if (c == NULL) {
    return;
  }
  c->conn = NULL;
  c->stream = NULL;
  clear(&c->lt);
  clear(&c->md);
  if (!c->q->closing) {
    bk_sut_on_disconnect(c->q->sut, c->index);
  }
}

static lsquic_stream_ctx_t *on_new_stream(void *ctx, lsquic_stream_t *s) {
  const lsq_t *q = ctx;
  if (s == NULL) {
    return NULL;
  }
  conn_t *c = conn_of(lsquic_stream_conn(s));
  c->stream = s;
  lsquic_stream_wantread(s, 1);
  if (!q->u.server) {
    bk_sut_on_connect(q->sut, c->index);
  }
  return (lsquic_stream_ctx_t *)c;
}

static void on_read(lsquic_stream_t *s, lsquic_stream_ctx_t *h) {
  conn_t *c = (conn_t *)h;
  for (;;) {
    uint32_t len = 0;
    if (c->rx_len >= sizeof(len)) {
      memcpy(&len, c->rx, sizeof(len));
      len = ntohl(len);
    }
    const size_t need = sizeof(len) + len;
    if (need > sizeof(c->rx)) {
      lsquic_conn_abort(c->conn);
      return;
    }
    if (c->rx_len >= sizeof(len) && c->rx_len == need) {
      bk_sut_on_receive(c->q->sut, c->index, c->rx + sizeof(len), len);
      c->rx_len = 0;
      continue;
    }
    const ssize_t n = lsquic_stream_read(s, c->rx + c->rx_len, need - c->rx_len);
    if (n < 0 && errno == EWOULDBLOCK) {
      return;
    }
    if (n <= 0) {
      lsquic_stream_wantread(s, 0);
      return;
    }
    c->rx_len += (size_t)n;
  }
}

static void on_write(lsquic_stream_t *s, lsquic_stream_ctx_t *h) {
  conn_t *c = (conn_t *)h;
  while (c->md.head != NULL) {
    msg *m = c->md.head;
    const ssize_t n = lsquic_stream_write(s, m->data + m->off, m->len - m->off);
    if (n <= 0) {
      break;
    }
    m->off += (size_t)n;
    if (m->off < m->len) {
      break;
    }
    free(pop(&c->md));
  }
  if (c->md.head == NULL) {
    lsquic_stream_wantwrite(s, 0);
  }
  lsquic_stream_flush(s);
}

static void on_close(lsquic_stream_t *s, lsquic_stream_ctx_t *h) {
  (void)s;
  conn_t *c = (conn_t *)h;
  c->stream = NULL;
}

static ssize_t on_dg_write(lsquic_conn_t *conn, void *buf, size_t sz) {
  conn_t *c = conn_of(conn);
  msg *m = c->lt.head;
  // 残りの空きに入らなければ -1 を返し、lsquic は次の packet で取りに来る
  if (m == NULL || m->len > sz) {
    return -1;
  }
  const size_t len = m->len;
  memcpy(buf, m->data, len);
  free(pop(&c->lt));
  if (c->lt.head == NULL) {
    lsquic_conn_want_datagram_write(conn, 0);
  }
  return (ssize_t)len;
}

static void on_datagram(lsquic_conn_t *conn, const void *buf, size_t len) {
  const conn_t *c = conn_of(conn);
  bk_sut_on_receive(c->q->sut, c->index, buf, len);
}

static const struct lsquic_stream_if stream_if = {
    .on_new_conn = on_new_conn,
    .on_conn_closed = on_conn_closed,
    .on_new_stream = on_new_stream,
    .on_read = on_read,
    .on_write = on_write,
    .on_close = on_close,
    .on_dg_write = on_dg_write,
    .on_datagram = on_datagram,
    .on_hsk_done = on_hsk_done,
};

// 送れなかった packet は回線で落ちたのと同じに扱い、すべて送ったと返す
static int packets_out(void *ctx, const struct lsquic_out_spec *specs,
                       unsigned n) {
  lsq_t *q = ctx;
  unsigned char buf[UDP_CONNS_MAX_DATAGRAM];
  for (unsigned i = 0; i < n; ++i) {
    size_t len = 0;
    for (size_t k = 0; k < specs[i].iovlen; ++k) {
      const struct iovec *v = &specs[i].iov[k];
      if (len + v->iov_len > sizeof(buf)) {
        break;
      }
      memcpy(buf + len, v->iov_base, v->iov_len);
      len += v->iov_len;
    }
    const conn_t *c = specs[i].peer_ctx;
    udp_conns_send(&q->u, c->index, buf, len);
  }
  return (int)n;
}

static void on_packet(void *ctx, int conn, bool is_new,
                      const unsigned char *data, size_t len) {
  (void)is_new;
  lsq_t *q = ctx;
  const struct sockaddr_in *local = &q->local[q->u.server ? 0 : conn];
  const struct sockaddr_in *peer = q->u.server ? &q->u.peers[conn] : &q->server;
  lsquic_engine_packet_in(q->engine, data, len, (const struct sockaddr *)local,
                          (const struct sockaddr *)peer, &q->conns[conn], 0);
}

static int select_alpn(SSL *ssl, const uint8_t **out, uint8_t *out_len,
                       const uint8_t *in, unsigned in_len, void *arg) {
  (void)ssl;
  (void)arg;
  return SSL_select_next_proto((uint8_t **)out, out_len, kAlpn,
                               sizeof(kAlpn) - 1u, in,
                               in_len) == OPENSSL_NPN_NEGOTIATED
             ? SSL_TLSEXT_ERR_OK
             : SSL_TLSEXT_ERR_ALERT_FATAL;
}

static SSL_CTX *get_ssl_ctx(void *peer_ctx, const struct sockaddr *local) {
  (void)local;
  return ((conn_t *)peer_ctx)->q->ssl;
}

// 自己署名の証明書をその場で作る(client は検証しない)
static SSL_CTX *server_ssl_ctx(void) {
  SSL_CTX *ctx = SSL_CTX_new(TLS_method());
  EVP_PKEY *key = EVP_PKEY_new();
  EC_KEY *ec = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
  X509 *cert = X509_new();
  bool ok = ctx != NULL && key != NULL && ec != NULL && cert != NULL &&
            EC_KEY_generate_key(ec) && EVP_PKEY_assign_EC_KEY(key, ec);
  if (ok) {
    ec = NULL;  // key が持つ
    ok = X509_set_version(cert, X509_VERSION_3) &&
         ASN1_INTEGER_set(X509_get_serialNumber(cert), 1) &&
         X509_gmtime_adj(X509_getm_notBefore(cert), 0) &&
         X509_gmtime_adj(X509_getm_notAfter(cert), 86400) &&
         X509_set_pubkey(cert, key) && X509_sign(cert, key, EVP_sha256()) &&
         SSL_CTX_use_certificate(ctx, cert) &&
         SSL_CTX_use_PrivateKey(ctx, key);
  }
  if (ok) {
    SSL_CTX_set_alpn_select_cb(ctx, select_alpn, NULL);
  } else {
    SSL_CTX_free(ctx);
    ctx = NULL;
  }
  X509_free(cert);
  EC_KEY_free(ec);
  EVP_PKEY_free(key);
  return ctx;
}

static void lsq_close(void *t) {
  lsq_t *q = t;
  q->closing = true;
  if (q->engine != NULL) {
    lsquic_engine_destroy(q->engine);
  }
  for (int i = 0; q->conns != NULL && i < q->n; ++i) {
    clear(&q->conns[i].lt);
    clear(&q->conns[i].md);
  }
  SSL_CTX_free(q->ssl);
  udp_conns_close(&q->u);
  free(q->local);
  free(q->conns);
  free(q);
  lsquic_global_cleanup();
}

static lsq_t *create(bk_sut *sut, bool server, int n) {
  if (lsquic_global_init(server ? LSQUIC_GLOBAL_SERVER : LSQUIC_GLOBAL_CLIENT) !=
      0) {
    return NULL;
  }
  lsq_t *q = calloc(1, sizeof(*q));
  if (q == NULL) {
    lsquic_global_cleanup();
    return NULL;
  }
  q->sut = sut;
  q->n = n;
  q->conns = calloc((size_t)n, sizeof(*q->conns));
  q->local = calloc((size_t)n, sizeof(*q->local));
  q->ssl = server ? server_ssl_ctx() : NULL;
  const unsigned flags = server ? LSENG_SERVER : 0;
  struct lsquic_engine_settings settings;
  lsquic_engine_init_settings(&settings, flags);
  settings.es_datagrams = 1;
  const struct lsquic_engine_api api = {
      .ea_settings = &settings,
      .ea_stream_if = &stream_if,
      .ea_stream_if_ctx = q,
      .ea_packets_out = packets_out,
      .ea_packets_out_ctx = q,
      .ea_get_ssl_ctx = get_ssl_ctx,
      .ea_alpn = (const char *)kAlpn + 1,
  };
  if (q->conns == NULL || q->local == NULL || (server && q->ssl == NULL) ||
      (q->engine = lsquic_engine_new(flags, &api)) == NULL) {
    lsq_close(q);
    return NULL;
  }
  for (int i = 0; i < n; ++i) {
    q->conns[i].q = q;
    q->conns[i].index = i;
  }
  return q;
}

static void *lsq_listen(bk_sut *sut, uint16_t port, int max_conns) {
  lsq_t *q = create(sut, true, max_conns);
  if (q == NULL) {
    return NULL;
  }
  if (udp_conns_listen(&q->u, port, max_conns) != 0) {
    lsq_close(q);
    return NULL;
  }
  q->local[0] = (struct sockaddr_in){
      .sin_family = AF_INET,
      .sin_port = htons(port),
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  return q;
}

static void *lsq_connect(bk_sut *sut, const char *host, uint16_t port,
                         int conns) {
  lsq_t *q = create(sut, false, conns);
  if (q == NULL) {
    return NULL;
  }
  q->server = (struct sockaddr_in){.sin_family = AF_INET,
                                   .sin_port = htons(port)};
  bool ok = inet_pton(AF_INET, host, &q->server.sin_addr) == 1 &&
            udp_conns_connect(&q->u, host, port, conns) == 0;
  for (int i = 0; ok && i < conns; ++i) {
    socklen_t len = sizeof(q->local[i]);
    conn_t *c = &q->conns[i];
    ok = getsockname(q->u.pfds[i].fd, (struct sockaddr *)&q->local[i], &len) ==
             0 &&
         lsquic_engine_connect(q->engine, N_LSQVER,
                               (const struct sockaddr *)&q->local[i],
                               (const struct sockaddr *)&q->server, c,
                               (lsquic_conn_ctx_t *)c, NULL, 0, NULL, 0, NULL,
                               0) != NULL;
  }
  if (!ok) {
    lsq_close(q);
    return NULL;
  }
  return q;
}

static int lsq_send(void *t, int conn, bool must_deliver, const void *data,
                    size_t len) {
  lsq_t *q = t;
  conn_t *c = &q->conns[conn];
  if (c->conn == NULL || (must_deliver && c->stream == NULL)) {
    return -1;
  }
  const size_t prefix = must_deliver ? sizeof(uint32_t) : 0;
  msg *m = malloc(sizeof(*m) + prefix + len);
  if (m == NULL) {
    return -1;
  }
  *m = (msg){.len = prefix + len};
  if (must_deliver) {
    const uint32_t nlen = htonl((uint32_t)len);
    memcpy(m->data, &nlen, sizeof(nlen));
  }
  memcpy(m->data + prefix, data, len);
  if (must_deliver) {
    push(&c->md, m);
    lsquic_stream_wantwrite(c->stream, 1);
  } else if (lsquic_conn_want_datagram_write(c->conn, 1) >= 0) {
    push(&c->lt, m);
  } else {
    free(m);
    return -1;
  }
  return 0;
}

static int lsq_service(void *t, uint64_t timeout_ns) {
  lsq_t *q = t;
  lsquic_engine_process_conns(q->engine);
  // lsquic_engine_earliest_adv_tick が示す次の処理の時刻より長くは待たない
  int wait_ms = (int)(timeout_ns / 1000000u);
  int diff_us = 0;
  if (lsquic_engine_earliest_adv_tick(q->engine, &diff_us) &&
      diff_us / 1000 < wait_ms) {
    wait_ms = diff_us > 0 ? diff_us / 1000 : 0;
  }
  if (udp_conns_poll(&q->u, wait_ms, on_packet, q) != 0) {
    return -1;
  }
  lsquic_engine_process_conns(q->engine);
  return 0;
}

static const bk_transport transport = {
    .name = "lsquic",
    .describe =
        "\"class_mapping\":{"
        "\"loss_tolerant\":{\"primitive\":\"quic datagram\","
        "\"delivery\":\"best_effort\",\"ordering\":\"unordered\","
        "\"realization\":\"native\"},"
        "\"must_deliver\":{\"primitive\":\"quic stream\","
        "\"delivery\":\"reliable\",\"ordering\":\"ordered\","
        "\"realization\":\"native\"}},"
        "\"coalescing\":\"none\",\"cc_algo\":\"bbr v1 (default)\","
        "\"thread_model\":\"single\",\"encryption\":true,"
        "\"tuning\":["
        "{\"knob\":\"es_datagrams\",\"value\":\"1\","
        "\"upstream_ref\":\"docs/apiref.rst\"}]",
    .max_payload = MAX_PAYLOAD,
    .listen = lsq_listen,
    .connect = lsq_connect,
    .send = lsq_send,
    .service = lsq_service,
    .close = lsq_close,
};

int main(int argc, char **argv) { return bk_sut_main(argc, argv, &transport); }
