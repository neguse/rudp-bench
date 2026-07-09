#include "benchkit.h"
#include "msquic_common.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using rudp_bench_msquic::FrameDecoder;
using rudp_bench_msquic::MsQuic;
using rudp_bench_msquic::SendBuf;

constexpr uint32_t kServiceSliceMs = 10;

enum ClassIndex {
  CLASS_LOSS_TOLERANT = 0,
  CLASS_MUST_DELIVER = 1,
  CLASS_COUNT = 2
};

enum DistIndex {
  DIST_ECHO = 0,
  DIST_BROADCAST = 1,
  DIST_COUNT = 2
};

// カウンタは atomic: handle_payload は msquic worker スレッド(複数 partition)
// の callback から並行に走るため、単一 mutex だと全 worker がメッセージごとに
// 直列化される。
struct ServerStats {
  std::atomic<uint64_t> recv[CLASS_COUNT][DIST_COUNT] = {};
  std::atomic<uint64_t> recv_measured[CLASS_COUNT][DIST_COUNT] = {};
  std::atomic<uint64_t> submit[CLASS_COUNT][DIST_COUNT] = {};
  std::atomic<uint64_t> submit_measured[CLASS_COUNT][DIST_COUNT] = {};
  std::atomic<uint64_t> send_failed[CLASS_COUNT][DIST_COUNT] = {};
  std::atomic<uint64_t> invalid_payload = {};
};

class ServerApp;

struct ConnState {
  ServerApp *app;
  HQUIC conn;
  std::atomic<HQUIC> stream = {nullptr};
  uint64_t id;
  std::atomic<bool> connected = {false};
};

struct StreamCtx {
  ConnState *conn;
  HQUIC stream;
  FrameDecoder decoder;
  std::mutex mu;
};

volatile sig_atomic_t g_stop = 0;

void on_signal(int /*signo*/) { g_stop = 1; }

ClassIndex class_from_flags(uint8_t flags) {
  return (flags & BK_FLAG_MUST_DELIVER) != 0 ? CLASS_MUST_DELIVER
                                             : CLASS_LOSS_TOLERANT;
}

DistIndex dist_from_flags(uint8_t flags) {
  return (flags & BK_FLAG_BROADCAST) != 0 ? DIST_BROADCAST : DIST_ECHO;
}

int parse_u16(const char *s, uint16_t *out) {
  if (s == nullptr || *s == '\0') {
    return -1;
  }
  errno = 0;
  char *end = nullptr;
  const unsigned long v = std::strtoul(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0' || v > UINT16_MAX) {
    return -1;
  }
  *out = static_cast<uint16_t>(v);
  return 0;
}

void usage(const char *argv0) {
  std::fprintf(stderr, "usage: %s --port PORT\n", argv0);
}

int parse_args(int argc, char **argv, uint16_t *port) {
  bool have_port = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--describe") == 0) {
      rudp_bench_msquic::print_describe();
      std::exit(EXIT_SUCCESS);
    }
    if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
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

int write_metrics_out(const char *json) {
  const char *path = std::getenv("BENCH_METRICS_OUT");
  if (path == nullptr || *path == '\0') {
    return 0;
  }
  FILE *f = std::fopen(path, "w");
  if (f == nullptr) {
    return -1;
  }
  const int rc =
      std::fputs(json, f) == EOF || std::fputc('\n', f) == EOF ? -1 : 0;
  if (std::fclose(f) != 0) {
    return -1;
  }
  return rc;
}

class ServerApp {
 public:
  bool start(uint16_t port) {
    rudp_bench_msquic::ensure_msquic();
    const auto &cert_paths = rudp_bench_msquic::ensure_self_signed_cert();

    QUIC_REGISTRATION_CONFIG reg_cfg = {"rudp-bench-v2-msquic-server",
                                        QUIC_EXECUTION_PROFILE_LOW_LATENCY};
    if (!rudp_bench_msquic::status_ok(
            MsQuic->RegistrationOpen(&reg_cfg, &registration_),
            "RegistrationOpen")) {
      return false;
    }

    QUIC_SETTINGS settings;
    rudp_bench_msquic::apply_common_settings(&settings);
    if (!rudp_bench_msquic::status_ok(
            MsQuic->ConfigurationOpen(registration_, rudp_bench_msquic::alpn(),
                                      1, &settings, sizeof(settings), nullptr,
                                      &configuration_),
            "ConfigurationOpen")) {
      return false;
    }

    QUIC_CERTIFICATE_FILE cert_file = {};
    cert_file.PrivateKeyFile = cert_paths.key;
    cert_file.CertificateFile = cert_paths.cert;
    QUIC_CREDENTIAL_CONFIG cred_cfg = {};
    cred_cfg.Type = QUIC_CREDENTIAL_TYPE_CERTIFICATE_FILE;
    cred_cfg.CertificateFile = &cert_file;
    cred_cfg.Flags = QUIC_CREDENTIAL_FLAG_NONE;
    if (!rudp_bench_msquic::status_ok(
            MsQuic->ConfigurationLoadCredential(configuration_, &cred_cfg),
            "ConfigurationLoadCredential")) {
      return false;
    }

    if (!rudp_bench_msquic::status_ok(
            MsQuic->ListenerOpen(registration_, listener_cb, this, &listener_),
            "ListenerOpen")) {
      return false;
    }

    QUIC_ADDR addr = {};
    QuicAddrSetFamily(&addr, QUIC_ADDRESS_FAMILY_UNSPEC);
    QuicAddrSetPort(&addr, port);
    return rudp_bench_msquic::status_ok(
        MsQuic->ListenerStart(listener_, rudp_bench_msquic::alpn(), 1, &addr),
        "ListenerStart");
  }

  QUIC_STATUS on_listener_event(QUIC_LISTENER_EVENT *event) {
    if (event->Type != QUIC_LISTENER_EVENT_NEW_CONNECTION) {
      return QUIC_STATUS_SUCCESS;
    }

    auto *conn = new ConnState();
    conn->app = this;
    conn->conn = event->NEW_CONNECTION.Connection;
    {
      // conns_ の更新は稀(接続/切断時のみ)。読み手(broadcast fanout、
      // メッセージごと)は copy-on-write な snapshot を atomic_load で取る
      std::lock_guard<std::mutex> lock(conns_mu_);
      conn->id = next_conn_id_++;
      auto next =
          std::make_shared<std::vector<ConnState *>>(*std::atomic_load(&conns_));
      next->push_back(conn);
      std::atomic_store(
          &conns_,
          std::shared_ptr<const std::vector<ConnState *>>(std::move(next)));
    }

    MsQuic->SetCallbackHandler(conn->conn,
                               reinterpret_cast<void *>(connection_cb), conn);
    return MsQuic->ConnectionSetConfiguration(conn->conn, configuration_);
  }

  QUIC_STATUS on_connection_event(ConnState *conn,
                                  QUIC_CONNECTION_EVENT *event) {
    switch (event->Type) {
      case QUIC_CONNECTION_EVENT_CONNECTED:
        conn->connected.store(true, std::memory_order_release);
        break;

      case QUIC_CONNECTION_EVENT_PEER_STREAM_STARTED:
        accept_peer_stream(conn, event->PEER_STREAM_STARTED.Stream);
        break;

      case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED:
        handle_payload(conn, event->DATAGRAM_RECEIVED.Buffer->Buffer,
                       event->DATAGRAM_RECEIVED.Buffer->Length);
        break;

      case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED:
        rudp_bench_msquic::handle_datagram_send_state(event);
        break;

      case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        conn->connected.store(false, std::memory_order_release);
        conn->stream.store(nullptr, std::memory_order_release);
        break;

      default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
  }

  QUIC_STATUS on_stream_event(StreamCtx *ctx, HQUIC stream,
                              QUIC_STREAM_EVENT *event) {
    switch (event->Type) {
      case QUIC_STREAM_EVENT_RECEIVE:
        {
          std::lock_guard<std::mutex> lock(ctx->mu);
          ctx->decoder.append(
              event->RECEIVE.Buffers, event->RECEIVE.BufferCount,
              [&](const uint8_t *data, size_t len) {
                ctx->conn->app->handle_payload(ctx->conn, data, len);
              });
        }
        break;

      case QUIC_STREAM_EVENT_SEND_COMPLETE:
        rudp_bench_msquic::handle_stream_send_complete(event);
        break;

      case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        {
          HQUIC expected = stream;
          ctx->conn->stream.compare_exchange_strong(expected, nullptr);
        }
        MsQuic->StreamClose(stream);
        delete ctx;
        return QUIC_STATUS_SUCCESS;

      default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
  }

  void handle_payload(ConnState *origin, const uint8_t *data, size_t len) {
    bk_header header;
    if (bk_payload_read(data, len, &header) != 0) {
      stats_.invalid_payload.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    const ClassIndex cls = class_from_flags(header.flags);
    const DistIndex dist = dist_from_flags(header.flags);
    const bool measured = (header.flags & BK_FLAG_MEASURE) != 0;
    stats_.recv[cls][dist].fetch_add(1, std::memory_order_relaxed);
    if (measured) {
      stats_.recv_measured[cls][dist].fetch_add(1, std::memory_order_relaxed);
    }

    if (dist == DIST_ECHO) {
      const bool ok = send_by_flags(origin, data, len, header.flags) == 0;
      count_submit(cls, dist, measured, ok);
      return;
    }

    // broadcast: frame バイト列を SendBuf で 1 回だけ構築し、全 target で
    // refcount 共有する(per-target の malloc+copy を排除)。class は header
    // 由来で全 target 共通なので framing も 1 種で足りる。
    const bool is_md = (header.flags & BK_FLAG_MUST_DELIVER) != 0;
    const auto targets = std::atomic_load(&conns_);
    const int n_targets = static_cast<int>(targets->size());
    if (n_targets == 0) {
      return;
    }
    SendBuf *buf = nullptr;
    if (is_md) {
      buf = rudp_bench_msquic::send_buf_new(sizeof(uint32_t) + len, n_targets);
      if (buf != nullptr) {
        const uint32_t nlen = htonl(static_cast<uint32_t>(len));
        std::memcpy(buf->data(), &nlen, sizeof(nlen));
        std::memcpy(buf->data() + sizeof(nlen), data, len);
      }
    } else {
      buf = rudp_bench_msquic::send_buf_new(len, n_targets);
      if (buf != nullptr) {
        std::memcpy(buf->data(), data, len);
      }
    }
    if (buf == nullptr) {
      for (int i = 0; i < n_targets; ++i) {
        count_submit(cls, dist, measured, false);
      }
      return;
    }
    for (ConnState *target : *targets) {
      if (!target->connected.load(std::memory_order_acquire)) {
        rudp_bench_msquic::send_buf_unref(buf);
        count_submit(cls, dist, measured, false);
        continue;
      }
      bool ok = false;
      if (is_md) {
        const HQUIC stream = target->stream.load(std::memory_order_acquire);
        ok = stream != nullptr &&
             rudp_bench_msquic::send_stream_buf(stream, buf) == 0;
        if (stream == nullptr) {
          rudp_bench_msquic::send_buf_unref(buf);
        }
      } else {
        ok = rudp_bench_msquic::send_datagram_buf(target->conn, buf) == 0;
      }
      count_submit(cls, dist, measured, ok);
    }
  }

  int format_stats_json(char *buf, size_t cap) {
    struct {
      uint64_t recv[CLASS_COUNT][DIST_COUNT];
      uint64_t recv_measured[CLASS_COUNT][DIST_COUNT];
      uint64_t submit[CLASS_COUNT][DIST_COUNT];
      uint64_t submit_measured[CLASS_COUNT][DIST_COUNT];
      uint64_t send_failed[CLASS_COUNT][DIST_COUNT];
      uint64_t invalid_payload;
    } s;
    for (int c = 0; c < CLASS_COUNT; ++c) {
      for (int d = 0; d < DIST_COUNT; ++d) {
        s.recv[c][d] = stats_.recv[c][d].load(std::memory_order_relaxed);
        s.recv_measured[c][d] =
            stats_.recv_measured[c][d].load(std::memory_order_relaxed);
        s.submit[c][d] = stats_.submit[c][d].load(std::memory_order_relaxed);
        s.submit_measured[c][d] =
            stats_.submit_measured[c][d].load(std::memory_order_relaxed);
        s.send_failed[c][d] =
            stats_.send_failed[c][d].load(std::memory_order_relaxed);
      }
    }
    s.invalid_payload = stats_.invalid_payload.load(std::memory_order_relaxed);
    const int n = std::snprintf(
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
        s.recv[CLASS_LOSS_TOLERANT][DIST_ECHO],
        s.recv[CLASS_LOSS_TOLERANT][DIST_BROADCAST],
        s.recv[CLASS_MUST_DELIVER][DIST_ECHO],
        s.recv[CLASS_MUST_DELIVER][DIST_BROADCAST],
        s.submit[CLASS_LOSS_TOLERANT][DIST_ECHO],
        s.submit[CLASS_LOSS_TOLERANT][DIST_BROADCAST],
        s.submit[CLASS_MUST_DELIVER][DIST_ECHO],
        s.submit[CLASS_MUST_DELIVER][DIST_BROADCAST],
        s.recv_measured[CLASS_LOSS_TOLERANT][DIST_ECHO],
        s.recv_measured[CLASS_LOSS_TOLERANT][DIST_BROADCAST],
        s.recv_measured[CLASS_MUST_DELIVER][DIST_ECHO],
        s.recv_measured[CLASS_MUST_DELIVER][DIST_BROADCAST],
        s.submit_measured[CLASS_LOSS_TOLERANT][DIST_ECHO],
        s.submit_measured[CLASS_LOSS_TOLERANT][DIST_BROADCAST],
        s.submit_measured[CLASS_MUST_DELIVER][DIST_ECHO],
        s.submit_measured[CLASS_MUST_DELIVER][DIST_BROADCAST],
        s.send_failed[CLASS_LOSS_TOLERANT][DIST_ECHO],
        s.send_failed[CLASS_LOSS_TOLERANT][DIST_BROADCAST],
        s.send_failed[CLASS_MUST_DELIVER][DIST_ECHO],
        s.send_failed[CLASS_MUST_DELIVER][DIST_BROADCAST],
        s.invalid_payload);
    return (n > 0 && static_cast<size_t>(n) < cap) ? 0 : -1;
  }

 private:
  void accept_peer_stream(ConnState *conn, HQUIC stream) {
    auto *ctx = new StreamCtx();
    ctx->conn = conn;
    ctx->stream = stream;
    HQUIC expected = nullptr;
    conn->stream.compare_exchange_strong(expected, stream);
    MsQuic->SetCallbackHandler(stream, reinterpret_cast<void *>(stream_cb),
                               ctx);
  }

  int send_by_flags(ConnState *conn, const void *data, size_t len,
                    uint8_t flags) {
    if ((flags & BK_FLAG_MUST_DELIVER) == 0) {
      return rudp_bench_msquic::send_datagram_payload(conn->conn, data, len);
    }
    const HQUIC stream = conn->stream.load(std::memory_order_acquire);
    if (stream == nullptr) {
      return -1;
    }
    return rudp_bench_msquic::send_stream_frame(stream, data, len);
  }

  void count_submit(ClassIndex cls, DistIndex dist, bool measured, bool ok) {
    if (ok) {
      stats_.submit[cls][dist].fetch_add(1, std::memory_order_relaxed);
      if (measured) {
        stats_.submit_measured[cls][dist].fetch_add(1,
                                                    std::memory_order_relaxed);
      }
    } else {
      stats_.send_failed[cls][dist].fetch_add(1, std::memory_order_relaxed);
    }
  }

  static QUIC_STATUS QUIC_API listener_cb(HQUIC /*listener*/, void *ctx,
                                          QUIC_LISTENER_EVENT *event) {
    return static_cast<ServerApp *>(ctx)->on_listener_event(event);
  }

  static QUIC_STATUS QUIC_API connection_cb(HQUIC /*connection*/, void *ctx,
                                            QUIC_CONNECTION_EVENT *event) {
    return static_cast<ConnState *>(ctx)->app->on_connection_event(
        static_cast<ConnState *>(ctx), event);
  }

  static QUIC_STATUS QUIC_API stream_cb(HQUIC stream, void *ctx,
                                        QUIC_STREAM_EVENT *event) {
    return static_cast<StreamCtx *>(ctx)->conn->app->on_stream_event(
        static_cast<StreamCtx *>(ctx), stream, event);
  }

  HQUIC registration_ = nullptr;
  HQUIC configuration_ = nullptr;
  HQUIC listener_ = nullptr;

  std::mutex conns_mu_;  // conns_ の copy-on-write 更新のみ直列化
  std::shared_ptr<const std::vector<ConnState *>> conns_ =
      std::make_shared<const std::vector<ConnState *>>();
  uint64_t next_conn_id_ = 1;

  ServerStats stats_;
};

}  // namespace

int main(int argc, char **argv) {
  uint16_t port = 0;
  if (parse_args(argc, argv, &port) != 0) {
    usage(argv[0]);
    return EXIT_FAILURE;
  }

  if (std::signal(SIGINT, on_signal) == SIG_ERR ||
      std::signal(SIGTERM, on_signal) == SIG_ERR) {
    std::perror("signal");
    return EXIT_FAILURE;
  }

  auto *app = new ServerApp();
  if (!app->start(port)) {
    return EXIT_FAILURE;
  }

  bk_control *control = bk_control_connect(nullptr);
  bk_schedule schedule = {0, 0, 0};
  if (control != nullptr) {
    if (bk_control_hello(control, "server", "msquic", 0) != 0 ||
        bk_control_ready(control, 0) != 0) {
      std::fprintf(stderr, "benchkit control handshake failed\n");
      bk_control_close(control);
      return EXIT_FAILURE;
    }
    for (;;) {
      const int r = bk_control_poll_schedule(control, &schedule);
      if (r == 1) {
        break;
      }
      if (r < 0 || g_stop) {
        std::fprintf(stderr, "benchkit schedule wait failed\n");
        bk_control_close(control);
        return EXIT_FAILURE;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kServiceSliceMs));
    }
  }

  bool window_final = false;
  while (!g_stop) {
    if (control != nullptr) {
      const uint64_t now = bk_now_ns();
      // 定常判定つき warmup(benchspec v2): 確定窓(window)を受けたら
      // schedule を差し替える(drain 終端の前倒しに効く)
      if (!window_final) {
        if (now >= schedule.start_at_ns) {
          window_final = true;
        } else {
          const int wr = bk_control_poll_window(control, &schedule);
          if (wr < 0) {
            std::fprintf(stderr, "benchkit window poll failed\n");
            bk_control_close(control);
            return EXIT_FAILURE;
          }
          if (wr == 1) {
            window_final = true;
          }
        }
      }
      if (now >= schedule.drain_until_ns) {
        break;
      }
      uint64_t sleep_ms = kServiceSliceMs;
      const uint64_t remain_ms = (schedule.drain_until_ns - now) / 1000000ull;
      if (remain_ms < sleep_ms) {
        sleep_ms = remain_ms;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
    } else {
      std::this_thread::sleep_for(std::chrono::milliseconds(kServiceSliceMs));
    }
  }

  char stats_json[4096];
  if (app->format_stats_json(stats_json, sizeof(stats_json)) != 0) {
    std::fprintf(stderr, "server stats JSON overflow\n");
    if (control != nullptr) {
      bk_control_close(control);
    }
    return EXIT_FAILURE;
  }

  if (write_metrics_out(stats_json) != 0) {
    std::fprintf(stderr, "failed to write BENCH_METRICS_OUT\n");
  }

  if (control != nullptr) {
    if (bk_control_done(control, stats_json) != 0) {
      std::fprintf(stderr, "benchkit done failed\n");
      bk_control_close(control);
      return EXIT_FAILURE;
    }
    bk_control_close(control);
  }

  return EXIT_SUCCESS;
}
