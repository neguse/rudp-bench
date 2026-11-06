// ENet の層。LT は unreliable(既定の sequenced)、MD は reliable で送り、
// 互いに待たされないよう別のチャネルに載せる(docs/design.dox の
// 「Sequencing」「Channels」)。
//
// ENET_LEGACY を定義すると、2026-11-05 の測定と同じ設定になる。
#include "bk_sut.h"

#include <enet/enet.h>

#include <stdint.h>
#include <stdlib.h>

#define CHANNEL_MD 0
#define CHANNEL_LT 1
#define CHANNELS 2
#define EVENT_BUDGET 4096

typedef struct {
  ENetHost *host;
  bk_sut *sut;
} enet_t;

#ifdef ENET_LEGACY
#define NAME "enet-legacy"
#define LT_FLAGS (ENET_PACKET_FLAG_UNSEQUENCED | ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT)
#define LT_PRIMITIVE "unreliable unsequenced (channel 1)"
#define LT_ORDERING "unordered"
#define CC_ALGO "enet packet throttle with deceleration=0"
#define TUNING                                                              \
  "[{\"knob\":\"enet_peer_throttle_configure\",\"value\":\"deceleration=0\"," \
  "\"upstream_ref\":\"include/enet/enet.h\"},"                              \
  "{\"knob\":\"enet_peer_timeout\",\"value\":\"minimum=10s,maximum=60s\","  \
  "\"upstream_ref\":\"include/enet/enet.h\"},"                              \
  "{\"knob\":\"socket_buffers\",\"value\":\"4MiB\","                        \
  "\"upstream_ref\":\"include/enet/enet.h\"}]"
#else
#define NAME "enet"
#define LT_FLAGS 0
#define LT_PRIMITIVE "unreliable sequenced (channel 1)"
#define LT_ORDERING "ordered"
#define CC_ALGO "enet packet throttle (default)"
#define TUNING "[]"
#endif

static int conn_of(const enet_t *e, const ENetPeer *peer) {
  return (int)(peer - e->host->peers);
}

static void *create(bk_sut *sut, const ENetAddress *address, int peers) {
  if (enet_initialize() != 0) {
    return NULL;
  }
  enet_t *e = calloc(1, sizeof(*e));
  if (e == NULL) {
    return NULL;
  }
  e->sut = sut;
  e->host = enet_host_create(address, (size_t)peers, CHANNELS, 0, 0);
  if (e->host == NULL) {
    free(e);
    return NULL;
  }
#ifdef ENET_LEGACY
  enet_socket_set_option(e->host->socket, ENET_SOCKOPT_RCVBUF, 4 << 20);
  enet_socket_set_option(e->host->socket, ENET_SOCKOPT_SNDBUF, 4 << 20);
#endif
  return e;
}

static void enet_close(void *t) {
  enet_t *e = t;
  enet_host_destroy(e->host);
  free(e);
  enet_deinitialize();
}

static void *enet_listen(bk_sut *sut, uint16_t port, int max_conns) {
  const ENetAddress address = {.host = ENET_HOST_ANY, .port = port};
  return create(sut, &address, max_conns);
}

static void *enet_connect(bk_sut *sut, const char *host, uint16_t port,
                          int conns) {
  ENetAddress address = {.port = port};
  enet_t *e = create(sut, NULL, conns);
  if (e == NULL) {
    return NULL;
  }
  bool ok = enet_address_set_host(&address, host) == 0;
  for (int i = 0; ok && i < conns; ++i) {
    ok = enet_host_connect(e->host, &address, CHANNELS, 0) != NULL;
  }
  if (!ok) {
    enet_close(e);
    return NULL;
  }
  return e;
}

static int enet_send(void *t, int conn, bool must_deliver, const void *data,
                     size_t len) {
  enet_t *e = t;
  ENetPacket *packet = enet_packet_create(
      data, len, must_deliver ? ENET_PACKET_FLAG_RELIABLE : LT_FLAGS);
  if (packet == NULL) {
    return -1;
  }
  if (enet_peer_send(&e->host->peers[conn],
                     must_deliver ? CHANNEL_MD : CHANNEL_LT, packet) != 0) {
    enet_packet_destroy(packet);
    return -1;
  }
  return 0;
}

static void dispatch(enet_t *e, ENetEvent *event) {
  const int conn = conn_of(e, event->peer);
  switch (event->type) {
    case ENET_EVENT_TYPE_CONNECT:
#ifdef ENET_LEGACY
      enet_peer_throttle_configure(event->peer,
                                   ENET_PEER_PACKET_THROTTLE_INTERVAL,
                                   ENET_PEER_PACKET_THROTTLE_SCALE, 0);
      enet_peer_timeout(event->peer, ENET_PEER_TIMEOUT_LIMIT, 10000, 60000);
#endif
      bk_sut_on_connect(e->sut, conn);
      break;
    case ENET_EVENT_TYPE_RECEIVE:
      bk_sut_on_receive(e->sut, conn, event->packet->data,
                        event->packet->dataLength);
      enet_packet_destroy(event->packet);
      break;
    case ENET_EVENT_TYPE_DISCONNECT:
      bk_sut_on_disconnect(e->sut, conn);
      break;
    case ENET_EVENT_TYPE_NONE:
      break;
  }
}

// enet_host_service は 1 回に 1 イベントしか返さないので、溜まった分は
// enet_host_check_events で引く。
static int enet_service(void *t, uint64_t timeout_ns) {
  enet_t *e = t;
  ENetEvent event;
  int rc = enet_host_service(e->host, &event,
                             (enet_uint32)(timeout_ns / 1000000u));
  for (int handled = 0; rc > 0 && handled < EVENT_BUDGET; ++handled) {
    dispatch(e, &event);
    rc = enet_host_check_events(e->host, &event);
    if (rc == 0) {
      rc = enet_host_service(e->host, &event, 0);
    }
  }
  return rc < 0 ? -1 : 0;
}

static const bk_transport transport = {
    .name = NAME,
    .describe =
        "\"class_mapping\":{"
        "\"loss_tolerant\":{\"primitive\":\"" LT_PRIMITIVE "\","
        "\"delivery\":\"best_effort\",\"ordering\":\"" LT_ORDERING "\","
        "\"realization\":\"native\"},"
        "\"must_deliver\":{\"primitive\":\"reliable (channel 0)\","
        "\"delivery\":\"reliable\",\"ordering\":\"ordered\","
        "\"realization\":\"native\"}},"
        "\"coalescing\":\"none\",\"cc_algo\":\"" CC_ALGO "\","
        "\"thread_model\":\"single\",\"encryption\":false,"
        "\"tuning\":" TUNING,
    .max_payload = 4096,
    .listen = enet_listen,
    .connect = enet_connect,
    .send = enet_send,
    .service = enet_service,
    .close = enet_close,
};

int main(int argc, char **argv) { return bk_sut_main(argc, argv, &transport); }
