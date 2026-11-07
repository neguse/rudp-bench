// coop_rudp の層。rudp.h は LT と MD の使い分けを文書にしていないので、送り方の名前から選ぶ。
// LT は新しい値より後に届いた古い値を捨てる RUDP_UNRELIABLE_SEQUENCED(rudp.c の deliver_frame)、MD は再送して順番どおりに渡す RUDP_RELIABLE_ORDERED で、チャネルを分ける。
// 設定は 0(= rudp.c の既定)のままにし、接続数と使うチャネル数に合わせた大きさだけ指定する。
// ライブラリは接続の手順を持たない。client は最初の service で全接続をつながったことにし、server は接続ごとの最初の受信でつながったことにする。
#include "bk_sut.h"

#include "benchkit.h"
#include "coop_rudp/rudp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHANNEL_LT 0
#define CHANNEL_MD 1
#define MTU 1200u
// 1 packet に 1 frame の分だけ入る(rudp.c の frame_payload)
#define MAX_PAYLOAD \
  (MTU - RUDP_WIRE_PACKET_HEADER_BYTES - RUDP_WIRE_DATA_FRAME_HEADER_BYTES)
// rudp_endpoint_poll が 1 回に読む packet の数(rudp.c の RUDP_DEFAULT_RECV_BATCH)
#define RECV_BATCH 64u
// ライブラリは次の再送の時刻を返さないので、この間隔で flush して再送の期限を見る
#define MAX_WAIT_MS 1

typedef struct {
  bk_sut *sut;
  int fd;
  bool server;
  int n;
  int known;          // server: 見えた接続の数
  uint64_t id_base;   // client: 接続 i の conn_id は id_base + i + 1
  uint64_t *ids;
  rudp_conn **conns;
  bool *connected;
  bool announced;     // client: on_connect を出したか
  size_t last_batch;  // 直近の recv_batch が返した数
  rudp_endpoint *ep;
  unsigned char buf[2048];
} coop_t;

static int send_batch(void *user, const rudp_out_packet *packets,
                      size_t count) {
  const coop_t *c = user;
  for (size_t i = 0; i < count; ++i) {
    sendto(c->fd, packets[i].data, packets[i].len, 0,
           (const struct sockaddr *)packets[i].addr.data, packets[i].addr.len);
  }
  return (int)count;
}

static int recv_batch(void *user, rudp_in_packet *packets, size_t max_count) {
  coop_t *c = user;
  size_t n = 0;
  while (n < max_count) {
    struct sockaddr_in from;
    memset(&from, 0, sizeof(from));
    socklen_t from_len = sizeof(from);
    const ssize_t got = recvfrom(c->fd, packets[n].data, packets[n].cap, 0,
                                 (struct sockaddr *)&from, &from_len);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    packets[n].len = (size_t)got;
    memcpy(packets[n].addr.data, &from, sizeof(from));
    packets[n].addr.len = sizeof(from);
    ++n;
  }
  c->last_batch = n;
  return (int)n;
}

static uint64_t now_ns(void *user) {
  (void)user;
  return bk_now_ns();
}

static void coop_close(void *t) {
  coop_t *c = t;
  if (c->ep != NULL) {
    rudp_endpoint_destroy(c->ep);
  }
  if (c->fd >= 0) {
    close(c->fd);
  }
  free(c->ids);
  free(c->conns);
  free(c->connected);
  free(c);
}

static coop_t *coop_new(bk_sut *sut, bool server, int n) {
  coop_t *c = calloc(1, sizeof(*c));
  if (c == NULL) {
    return NULL;
  }
  c->sut = sut;
  c->server = server;
  c->n = n;
  c->ids = calloc((size_t)n, sizeof(*c->ids));
  c->conns = calloc((size_t)n, sizeof(*c->conns));
  c->connected = calloc((size_t)n, sizeof(*c->connected));
  c->fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
  const rudp_endpoint_config cfg = {
      .socket = {.user = c,
                 .send_batch = send_batch,
                 .recv_batch = recv_batch,
                 .now_ns = now_ns},
      .mtu = MTU,
      .max_conns = (uint32_t)n,
      .max_flows = 1,
      .max_channels = 2,
  };
  if (c->ids == NULL || c->conns == NULL || c->connected == NULL ||
      c->fd < 0 || rudp_endpoint_create(&c->ep, &cfg) != 0) {
    coop_close(c);
    return NULL;
  }
  return c;
}

static void *coop_listen(bk_sut *sut, uint16_t port, int max_conns) {
  coop_t *c = coop_new(sut, true, max_conns);
  if (c == NULL) {
    return NULL;
  }
  const struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = htons(port),
      .sin_addr.s_addr = htonl(INADDR_ANY),
  };
  if (bind(c->fd, (const struct sockaddr *)&addr, sizeof(addr)) != 0) {
    coop_close(c);
    return NULL;
  }
  return c;
}

static void *coop_connect(bk_sut *sut, const char *host, uint16_t port,
                          int conns) {
  coop_t *c = coop_new(sut, false, conns);
  if (c == NULL) {
    return NULL;
  }
  struct sockaddr_in srv;
  memset(&srv, 0, sizeof(srv));
  srv.sin_family = AF_INET;
  srv.sin_port = htons(port);
  rudp_addr peer;
  memset(&peer, 0, sizeof(peer));
  peer.len = sizeof(srv);
  // server は conn_id で接続を見分けるので、client のプロセスごとに変える
  c->id_base = (uint64_t)getpid() << 16;
  bool ok = inet_pton(AF_INET, host, &srv.sin_addr) == 1;
  memcpy(peer.data, &srv, sizeof(srv));
  for (int i = 0; ok && i < conns; ++i) {
    c->ids[i] = c->id_base + (uint64_t)i + 1u;
    ok = rudp_endpoint_connect(c->ep, &peer, c->ids[i], &c->conns[i]) == 0;
  }
  if (!ok) {
    coop_close(c);
    return NULL;
  }
  return c;
}

static int coop_send(void *t, int conn, bool must_deliver, const void *data,
                     size_t len) {
  const coop_t *c = t;
  const rudp_send_opts opts = {
      .channel_id = must_deliver ? CHANNEL_MD : CHANNEL_LT,
      .reliability =
          must_deliver ? RUDP_RELIABLE_ORDERED : RUDP_UNRELIABLE_SEQUENCED,
  };
  const rudp_send_result r = rudp_send(c->conns[conn], data, len, &opts);
  return r == RUDP_SEND_OK || r == RUDP_SEND_QUEUED ? 0 : -1;
}

static void coop_flush(void *t) {
  rudp_endpoint_flush(((coop_t *)t)->ep, bk_now_ns());
}

static int find_conn(const coop_t *c, uint64_t id) {
  if (!c->server) {
    return id > c->id_base && id - c->id_base <= (uint64_t)c->n
               ? (int)(id - c->id_base) - 1
               : -1;
  }
  for (int i = 0; i < c->known; ++i) {
    if (c->ids[i] == id) {
      return i;
    }
  }
  return -1;
}

static void drain(coop_t *c) {
  size_t len;
  rudp_recv_info info;
  while (rudp_recv(c->ep, c->buf, sizeof(c->buf), &len, &info) == 1) {
    int conn = find_conn(c, info.conn_id);
    if (conn < 0 && c->server && c->known < c->n) {
      conn = c->known++;
      c->ids[conn] = info.conn_id;
      c->conns[conn] = info.conn;
    }
    if (conn < 0) {
      continue;
    }
    if (!c->connected[conn]) {
      c->connected[conn] = true;
      bk_sut_on_connect(c->sut, conn);
    }
    bk_sut_on_receive(c->sut, conn, c->buf, len);
  }
}

static int coop_service(void *t, uint64_t timeout_ns) {
  coop_t *c = t;
  if (!c->server && !c->announced) {
    c->announced = true;
    for (int i = 0; i < c->n; ++i) {
      c->connected[i] = true;
      bk_sut_on_connect(c->sut, i);
    }
  }
  int wait_ms = (int)(timeout_ns / 1000000u);
  if (wait_ms > MAX_WAIT_MS) {
    wait_ms = MAX_WAIT_MS;
  }
  struct pollfd pfd = {.fd = c->fd, .events = POLLIN};
  if (poll(&pfd, 1, wait_ms) < 0 && errno != EINTR) {
    return -1;
  }
  do {
    rudp_endpoint_poll(c->ep, bk_now_ns());
  } while (c->last_batch >= RECV_BATCH);
  drain(c);
  rudp_endpoint_flush(c->ep, bk_now_ns());
  // max_retransmits か idle_timeout_ms を指定したときだけ、ライブラリが接続を閉じる
  for (int i = 0; i < (c->server ? c->known : c->n); ++i) {
    if (c->connected[i] && rudp_endpoint_find_conn(c->ep, c->ids[i]) == NULL) {
      c->connected[i] = false;
      bk_sut_on_disconnect(c->sut, i);
    }
  }
  return 0;
}

// 送信待ち + 送信済みで未確認 + 再送待ちの合計
static int64_t coop_send_queue(void *t, int conn) {
  const coop_t *c = t;
  if (c->conns[conn] == NULL) {
    return 0;
  }
  rudp_status st;
  rudp_get_status(c->conns[conn], &st);
  return (int64_t)st.send_queue_bytes + st.inflight_bytes +
         st.retransmit_queue_bytes;
}

static const bk_transport transport = {
    .name = "coop_rudp",
    .describe =
        "\"class_mapping\":{"
        "\"loss_tolerant\":{\"primitive\":\"RUDP_UNRELIABLE_SEQUENCED (channel 0)\","
        "\"delivery\":\"best_effort\",\"ordering\":\"ordered\","
        "\"realization\":\"native\"},"
        "\"must_deliver\":{\"primitive\":\"RUDP_RELIABLE_ORDERED (channel 1)\","
        "\"delivery\":\"reliable\",\"ordering\":\"ordered\","
        "\"realization\":\"native\"}},"
        "\"coalescing\":\"packet\","
        "\"cc_algo\":\"rate AIMD with token bucket pacing\","
        "\"thread_model\":\"single\",\"encryption\":false,"
        "\"tuning\":["
        "{\"knob\":\"max_conns\",\"value\":\"connections of the run (sizing)\","
        "\"upstream_ref\":\"coop_rudp/rudp.h\"},"
        "{\"knob\":\"max_flows\",\"value\":\"1 (sizing, flow 0 only)\","
        "\"upstream_ref\":\"coop_rudp/rudp.h\"},"
        "{\"knob\":\"max_channels\",\"value\":\"2 (sizing, LT and MD)\","
        "\"upstream_ref\":\"coop_rudp/rudp.h\"}]",
    .max_payload = MAX_PAYLOAD,
    .listen = coop_listen,
    .connect = coop_connect,
    .send = coop_send,
    .flush = coop_flush,
    .service = coop_service,
    .send_queue = coop_send_queue,
    .send_queue_unit = "bytes",
    .close = coop_close,
};

int main(int argc, char **argv) { return bk_sut_main(argc, argv, &transport); }
