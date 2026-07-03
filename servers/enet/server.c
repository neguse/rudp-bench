#include "benchkit.h"

#include <enet/enet.h>

#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ENET_BENCH_MAX_PAYLOAD_BYTES 65536u
#define ENET_BENCH_MAX_PEERS 4095u
#define ENET_CHANNEL_RELIABLE 0u
#define ENET_CHANNEL_UNRELIABLE 1u
#define ENET_SERVICE_SLICE_MS 10u

typedef enum {
  CLASS_LOSS_TOLERANT = 0,
  CLASS_MUST_DELIVER = 1,
  CLASS_COUNT = 2
} class_index;

typedef enum {
  DIST_ECHO = 0,
  DIST_BROADCAST = 1,
  DIST_COUNT = 2
} dist_index;

typedef struct {
  uint64_t recv[CLASS_COUNT][DIST_COUNT];
  uint64_t recv_measured[CLASS_COUNT][DIST_COUNT];
  uint64_t submit[CLASS_COUNT][DIST_COUNT];
  uint64_t submit_measured[CLASS_COUNT][DIST_COUNT];
  uint64_t send_failed[CLASS_COUNT][DIST_COUNT];
  uint64_t invalid_payload;
} server_stats;

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int signo) {
  (void)signo;
  g_stop = 1;
}

static void print_describe(void) {
  puts("{\"transport\":\"enet\","
       "\"class_mapping\":{\"loss_tolerant\":\"unreliable-unsequenced\","
       "\"must_deliver\":\"reliable\"},"
       "\"coalescing\":\"none\","
       "\"cc_algo\":\"enet-packet-throttle(scale=32,default=32,accel=2,decel=2,interval=5000ms)\","
       "\"thread_model\":\"single\","
       "\"encryption\":false,"
       "\"max_payload_bytes\":65536,"
       "\"tuning\":[]}");
}

static int parse_u16(const char *s, uint16_t *out) {
  if (s == NULL || *s == '\0') {
    return -1;
  }
  errno = 0;
  char *end = NULL;
  unsigned long v = strtoul(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0' || v > UINT16_MAX) {
    return -1;
  }
  *out = (uint16_t)v;
  return 0;
}

static void usage(const char *argv0) {
  fprintf(stderr, "usage: %s --port PORT\n", argv0);
}

static int parse_args(int argc, char **argv, uint16_t *port) {
  bool have_port = false;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--describe") == 0) {
      print_describe();
      exit(EXIT_SUCCESS);
    }
    if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      if (parse_u16(argv[++i], port) != 0 || *port == 0) {
        return -1;
      }
      have_port = true;
      continue;
    }
    return -1;
  }
  return have_port ? 0 : -1;
}

static class_index class_from_flags(uint8_t flags) {
  return (flags & BK_FLAG_MUST_DELIVER) != 0 ? CLASS_MUST_DELIVER
                                             : CLASS_LOSS_TOLERANT;
}

static dist_index dist_from_flags(uint8_t flags) {
  return (flags & BK_FLAG_BROADCAST) != 0 ? DIST_BROADCAST : DIST_ECHO;
}

static int enet_class_route(uint8_t flags, enet_uint8 *channel,
                            enet_uint32 *packet_flags) {
  if ((flags & BK_FLAG_MUST_DELIVER) != 0) {
    *channel = ENET_CHANNEL_RELIABLE;
    *packet_flags = ENET_PACKET_FLAG_RELIABLE;
  } else {
    *channel = ENET_CHANNEL_UNRELIABLE;
    *packet_flags = ENET_PACKET_FLAG_UNSEQUENCED;
  }
  return 0;
}

static int send_payload(ENetPeer *peer, const void *data, size_t len,
                        uint8_t flags) {
  enet_uint8 channel = 0;
  enet_uint32 packet_flags = 0;
  if (enet_class_route(flags, &channel, &packet_flags) != 0) {
    return -1;
  }
  ENetPacket *packet = enet_packet_create(data, len, packet_flags);
  if (packet == NULL) {
    return -1;
  }
  if (enet_peer_send(peer, channel, packet) != 0) {
    enet_packet_destroy(packet);
    return -1;
  }
  return 0;
}

static void count_submit(server_stats *stats, class_index cls, dist_index dist,
                         bool measured, bool ok) {
  if (ok) {
    stats->submit[cls][dist]++;
    if (measured) {
      stats->submit_measured[cls][dist]++;
    }
  } else {
    stats->send_failed[cls][dist]++;
  }
}

static void handle_receive(ENetHost *host, const ENetEvent *event,
                           server_stats *stats) {
  bk_header header;
  if (bk_payload_read(event->packet->data, event->packet->dataLength,
                      &header) != 0) {
    stats->invalid_payload++;
    return;
  }

  const class_index cls = class_from_flags(header.flags);
  const dist_index dist = dist_from_flags(header.flags);
  const bool measured = (header.flags & BK_FLAG_MEASURE) != 0;
  stats->recv[cls][dist]++;
  if (measured) {
    stats->recv_measured[cls][dist]++;
  }

  if (dist == DIST_ECHO) {
    const bool ok = send_payload(event->peer, event->packet->data,
                                 event->packet->dataLength, header.flags) == 0;
    count_submit(stats, cls, dist, measured, ok);
    return;
  }

  for (size_t i = 0; i < host->peerCount; ++i) {
    ENetPeer *peer = &host->peers[i];
    if (peer->state != ENET_PEER_STATE_CONNECTED) {
      continue;
    }
    const bool ok =
        send_payload(peer, event->packet->data, event->packet->dataLength,
                     header.flags) == 0;
    count_submit(stats, cls, dist, measured, ok);
  }
}

static int service_once(ENetHost *host, enet_uint32 timeout_ms,
                        server_stats *stats) {
  ENetEvent event;
  const int rc = enet_host_service(host, &event, timeout_ms);
  if (rc < 0) {
    return -1;
  }
  if (rc == 0) {
    return 0;
  }

  for (;;) {
    switch (event.type) {
      case ENET_EVENT_TYPE_CONNECT:
        break;
      case ENET_EVENT_TYPE_RECEIVE:
        handle_receive(host, &event, stats);
        enet_packet_destroy(event.packet);
        break;
      case ENET_EVENT_TYPE_DISCONNECT:
        event.peer->data = NULL;
        break;
      case ENET_EVENT_TYPE_NONE:
        break;
    }

    const int next = enet_host_service(host, &event, 0);
    if (next < 0) {
      return -1;
    }
    if (next == 0) {
      break;
    }
  }
  enet_host_flush(host);
  return 0;
}

static int format_stats_json(const server_stats *s, char *buf, size_t cap) {
  const int n = snprintf(
      buf, cap,
      "{\"recv\":{\"loss_tolerant\":{\"echo\":%" PRIu64
      ",\"broadcast\":%" PRIu64 "},\"must_deliver\":{\"echo\":%" PRIu64
      ",\"broadcast\":%" PRIu64
      "}},\"submit\":{\"loss_tolerant\":{\"echo\":%" PRIu64
      ",\"broadcast\":%" PRIu64 "},\"must_deliver\":{\"echo\":%" PRIu64
      ",\"broadcast\":%" PRIu64
      "}},\"recv_measured\":{\"loss_tolerant\":{\"echo\":%" PRIu64
      ",\"broadcast\":%" PRIu64 "},\"must_deliver\":{\"echo\":%" PRIu64
      ",\"broadcast\":%" PRIu64
      "}},\"submit_measured\":{\"loss_tolerant\":{\"echo\":%" PRIu64
      ",\"broadcast\":%" PRIu64 "},\"must_deliver\":{\"echo\":%" PRIu64
      ",\"broadcast\":%" PRIu64
      "}},\"send_failed\":{\"loss_tolerant\":{\"echo\":%" PRIu64
      ",\"broadcast\":%" PRIu64 "},\"must_deliver\":{\"echo\":%" PRIu64
      ",\"broadcast\":%" PRIu64 "}},\"invalid_payload\":%" PRIu64 "}",
      s->recv[CLASS_LOSS_TOLERANT][DIST_ECHO],
      s->recv[CLASS_LOSS_TOLERANT][DIST_BROADCAST],
      s->recv[CLASS_MUST_DELIVER][DIST_ECHO],
      s->recv[CLASS_MUST_DELIVER][DIST_BROADCAST],
      s->submit[CLASS_LOSS_TOLERANT][DIST_ECHO],
      s->submit[CLASS_LOSS_TOLERANT][DIST_BROADCAST],
      s->submit[CLASS_MUST_DELIVER][DIST_ECHO],
      s->submit[CLASS_MUST_DELIVER][DIST_BROADCAST],
      s->recv_measured[CLASS_LOSS_TOLERANT][DIST_ECHO],
      s->recv_measured[CLASS_LOSS_TOLERANT][DIST_BROADCAST],
      s->recv_measured[CLASS_MUST_DELIVER][DIST_ECHO],
      s->recv_measured[CLASS_MUST_DELIVER][DIST_BROADCAST],
      s->submit_measured[CLASS_LOSS_TOLERANT][DIST_ECHO],
      s->submit_measured[CLASS_LOSS_TOLERANT][DIST_BROADCAST],
      s->submit_measured[CLASS_MUST_DELIVER][DIST_ECHO],
      s->submit_measured[CLASS_MUST_DELIVER][DIST_BROADCAST],
      s->send_failed[CLASS_LOSS_TOLERANT][DIST_ECHO],
      s->send_failed[CLASS_LOSS_TOLERANT][DIST_BROADCAST],
      s->send_failed[CLASS_MUST_DELIVER][DIST_ECHO],
      s->send_failed[CLASS_MUST_DELIVER][DIST_BROADCAST],
      s->invalid_payload);
  return (n > 0 && (size_t)n < cap) ? 0 : -1;
}

static int write_metrics_out(const char *json) {
  const char *path = getenv("BENCH_METRICS_OUT");
  if (path == NULL || *path == '\0') {
    return 0;
  }
  FILE *f = fopen(path, "w");
  if (f == NULL) {
    return -1;
  }
  const int rc = fputs(json, f) == EOF || fputc('\n', f) == EOF ? -1 : 0;
  if (fclose(f) != 0) {
    return -1;
  }
  return rc;
}

int main(int argc, char **argv) {
  uint16_t port = 0;
  if (parse_args(argc, argv, &port) != 0) {
    usage(argv[0]);
    return EXIT_FAILURE;
  }

  if (signal(SIGINT, on_signal) == SIG_ERR ||
      signal(SIGTERM, on_signal) == SIG_ERR) {
    perror("signal");
    return EXIT_FAILURE;
  }

  if (enet_initialize() != 0) {
    fprintf(stderr, "enet_initialize failed\n");
    return EXIT_FAILURE;
  }
  atexit(enet_deinitialize);

  ENetAddress address;
  memset(&address, 0, sizeof(address));
  address.host = ENET_HOST_ANY;
  address.port = port;

  ENetHost *host = enet_host_create(&address, ENET_BENCH_MAX_PEERS, 2, 0, 0);
  if (host == NULL) {
    fprintf(stderr, "enet_host_create server failed on port %" PRIu16 "\n",
            port);
    return EXIT_FAILURE;
  }

  server_stats stats;
  memset(&stats, 0, sizeof(stats));

  bk_control *control = bk_control_connect(NULL);
  bk_schedule schedule = {0, 0, 0};
  if (control != NULL) {
    if (bk_control_hello(control, "server", "enet", 0) != 0 ||
        bk_control_ready(control, 0) != 0) {
      fprintf(stderr, "benchkit control handshake failed\n");
      bk_control_close(control);
      enet_host_destroy(host);
      return EXIT_FAILURE;
    }
    // schedule 待ちの間も service を回す。ここでブロックすると client の
    // 接続ハンドシェイクに応答できず、barrier 全体がデッドロックする。
    for (;;) {
      const int r = bk_control_poll_schedule(control, &schedule);
      if (r == 1) {
        break;
      }
      if (r < 0 || g_stop) {
        fprintf(stderr, "benchkit schedule wait failed\n");
        bk_control_close(control);
        enet_host_destroy(host);
        return EXIT_FAILURE;
      }
      if (service_once(host, ENET_SERVICE_SLICE_MS, &stats) != 0) {
        fprintf(stderr, "enet_host_service failed\n");
        bk_control_close(control);
        enet_host_destroy(host);
        return EXIT_FAILURE;
      }
    }
  }

  while (!g_stop) {
    enet_uint32 timeout_ms = ENET_SERVICE_SLICE_MS;
    if (control != NULL) {
      const uint64_t now = bk_now_ns();
      if (now >= schedule.drain_until_ns) {
        break;
      }
      const uint64_t remain_ns = schedule.drain_until_ns - now;
      const uint64_t remain_ms = remain_ns / 1000000ull;
      if (remain_ms < timeout_ms) {
        timeout_ms = (enet_uint32)remain_ms;
      }
    }
    if (service_once(host, timeout_ms, &stats) != 0) {
      fprintf(stderr, "enet_host_service failed\n");
      if (control != NULL) {
        bk_control_close(control);
      }
      enet_host_destroy(host);
      return EXIT_FAILURE;
    }
  }

  char stats_json[4096];
  if (format_stats_json(&stats, stats_json, sizeof(stats_json)) != 0) {
    fprintf(stderr, "server stats JSON overflow\n");
    if (control != NULL) {
      bk_control_close(control);
    }
    enet_host_destroy(host);
    return EXIT_FAILURE;
  }

  if (write_metrics_out(stats_json) != 0) {
    fprintf(stderr, "failed to write BENCH_METRICS_OUT\n");
  }

  if (control != NULL) {
    if (bk_control_done(control, stats_json) != 0) {
      fprintf(stderr, "benchkit done failed\n");
      bk_control_close(control);
      enet_host_destroy(host);
      return EXIT_FAILURE;
    }
    bk_control_close(control);
  }

  enet_host_destroy(host);
  return EXIT_SUCCESS;
}
