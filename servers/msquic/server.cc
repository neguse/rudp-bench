#include "benchkit.h"
#include "../scenario_cli.h"
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
constexpr uint64_t kDevWarmupNs = 200000000ull;
constexpr uint64_t kDevDurationNs = 2000000000ull;
constexpr uint64_t kDevDrainNs = 500000000ull;
constexpr uint32_t kMaxConns = 4095;

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
  std::atomic<uint64_t> server_state_ticks = {};
};

class ServerApp;

struct ConnState {
  ServerApp *app;
  HQUIC conn;
  std::atomic<HQUIC> stream = {nullptr};
  std::mutex stream_mu;
  uint64_t id;
  std::atomic<bool> connected = {false};
  std::atomic<int64_t> origin_id = {-1};
  std::atomic<uint64_t> applied_input_seq = {0};
};

struct ServerConfig {
  uint16_t port = 0;
  uint64_t staleness_period_ns = 10000000ull;
  bk_scenario_cli scenario = {};
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

int parse_args(int argc, char **argv, ServerConfig *cfg) {
  bool have_port = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--describe") == 0) {
      rudp_bench_msquic::print_describe();
      std::exit(EXIT_SUCCESS);
    }
    if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      if (parse_u16(argv[++i], &cfg->port) != 0 || cfg->port == 0) {
        return -1;
      }
      have_port = true;
      continue;
    }
    if (std::strcmp(argv[i], "--staleness-period-ns") == 0 &&
        i + 1 < argc) {
      if (bk_scenario_parse_u64(argv[++i], &cfg->staleness_period_ns) != 0 ||
          cfg->staleness_period_ns == 0) {
        return -1;
      }
      continue;
    }
    const int parsed =
        bk_scenario_cli_parse(argc, argv, &i, &cfg->scenario);
    if (parsed < 0) {
      return -1;
    }
    if (parsed > 0) {
      continue;
    }
    return -1;
  }
  return have_port &&
                 bk_scenario_cli_validate(
                     &cfg->scenario, kMaxConns,
                     rudp_bench_msquic::kMaxPayloadBytes) == 0
             ? 0
             : -1;
}

int write_metrics_out(const bk_metrics *metrics) {
  const char *path = std::getenv("BENCH_METRICS_OUT");
  if (path == nullptr || *path == '\0') {
    return 0;
  }
  return bk_metrics_dump_json(metrics, path);
}

int interval_from_rate(double rate_hz, uint64_t *out) {
  if (rate_hz <= 0.0) {
    return -1;
  }
  const double interval = 1000000000.0 / rate_hz;
  if (interval < 1.0 || interval > static_cast<double>(UINT64_MAX)) {
    return -1;
  }
  *out = static_cast<uint64_t>(interval + 0.5);
  if (*out == 0) {
    *out = 1;
  }
  return 0;
}

int build_state_streams(const ServerConfig &cfg, bk_stream *streams,
                        int *n_streams) {
  int n = 0;
  uint64_t interval_ns = 0;
  if (cfg.scenario.state.rate_lt > 0.0) {
    if (interval_from_rate(cfg.scenario.state.rate_lt, &interval_ns) != 0) {
      return -1;
    }
    streams[n].must_deliver = false;
    streams[n].broadcast = false;
    streams[n].traffic_id = cfg.scenario.state.traffic_id;
    streams[n].direction = BK_DIRECTION_SERVER_TO_CLIENT;
    streams[n].interval_ns = interval_ns;
    ++n;
  }
  if (cfg.scenario.state.rate_md > 0.0) {
    if (interval_from_rate(cfg.scenario.state.rate_md, &interval_ns) != 0) {
      return -1;
    }
    streams[n].must_deliver = true;
    streams[n].broadcast = false;
    streams[n].traffic_id = cfg.scenario.state.traffic_id;
    streams[n].direction = BK_DIRECTION_SERVER_TO_CLIENT;
    streams[n].interval_ns = interval_ns;
    ++n;
  }
  *n_streams = n;
  return n == 0 ? -1 : 0;
}

class ServerApp {
 public:
  explicit ServerApp(const ServerConfig &cfg) : cfg_(cfg) {}

  bool start(uint16_t port) {
    const bool authoritative =
        cfg_.scenario.kind == BK_SCENARIO_AUTHORITATIVE_STATE;
    bk_metrics_config metrics_cfg = {};
    metrics_cfg.max_origin_id =
        cfg_.scenario.present ? cfg_.scenario.total_conns + 1u : 1u;
    metrics_cfg.deadline_ns =
        authoritative ? cfg_.scenario.input.deadline_ns : 0;
    metrics_cfg.staleness_period_ns = cfg_.staleness_period_ns;
    metrics_cfg.max_local_index =
        authoritative ? cfg_.scenario.total_conns : 0;
    metrics_ = bk_metrics_new(&metrics_cfg);
    if (metrics_ == nullptr) {
      return false;
    }
    if (authoritative &&
        (bk_metrics_set_traffic_deadline(
             metrics_, cfg_.scenario.input.traffic_id,
             BK_DIRECTION_CLIENT_TO_SERVER,
             cfg_.scenario.input.deadline_ns) != 0 ||
         bk_metrics_set_traffic_deadline(
             metrics_, cfg_.scenario.state.traffic_id,
             BK_DIRECTION_SERVER_TO_CLIENT,
             cfg_.scenario.state.deadline_ns) != 0)) {
      return false;
    }
    if (authoritative) {
      roster_.assign(cfg_.scenario.total_conns, nullptr);
    }
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

  bool roster_complete() const {
    return cfg_.scenario.kind == BK_SCENARIO_AUTHORITATIVE_STATE &&
           registered_count_.load(std::memory_order_acquire) ==
               cfg_.scenario.total_conns;
  }

  bool freeze_roster() {
    std::lock_guard<std::mutex> lock(roster_mu_);
    if (registered_count_.load(std::memory_order_relaxed) !=
        cfg_.scenario.total_conns) {
      return false;
    }
    for (uint32_t origin = 0; origin < cfg_.scenario.total_conns; ++origin) {
      ConnState *conn = roster_[origin];
      if (conn == nullptr ||
          !conn->connected.load(std::memory_order_acquire)) {
        if (conn != nullptr) {
          conn->origin_id.store(-1, std::memory_order_release);
          conn->applied_input_seq.store(0, std::memory_order_relaxed);
          roster_[origin] = nullptr;
          registered_count_.fetch_sub(1, std::memory_order_relaxed);
        }
        return false;
      }
    }
    roster_frozen_.store(true, std::memory_order_release);
    return true;
  }

  int expect_client_inputs(const bk_schedule &schedule) {
    if (cfg_.scenario.input.rate_lt == 0.0) {
      return 0;
    }
    std::lock_guard<std::mutex> lock(metrics_mu_);
    for (uint32_t origin = 0; origin < cfg_.scenario.total_conns; ++origin) {
      if (bk_metrics_expect_latest(
              metrics_, origin, origin, cfg_.scenario.input.traffic_id,
              BK_DIRECTION_CLIENT_TO_SERVER, schedule.start_at_ns) != 0) {
        return -1;
      }
    }
    return 0;
  }

  void metrics_tick(uint64_t now) {
    std::lock_guard<std::mutex> lock(metrics_mu_);
    bk_metrics_tick(metrics_, now);
  }

  int dump_metrics_out() {
    std::lock_guard<std::mutex> lock(metrics_mu_);
    return write_metrics_out(metrics_);
  }

  void send_state_slot(const bk_slot &slot) {
    const bool must_deliver =
        (slot.flags & BK_FLAG_MUST_DELIVER) != 0;
    if (!must_deliver && (slot.flags & BK_FLAG_MEASURE) != 0) {
      stats_.server_state_ticks.fetch_add(1, std::memory_order_relaxed);
    }
    const size_t payload_size = must_deliver
                                    ? cfg_.scenario.state.payload_md
                                    : cfg_.scenario.state.payload_lt;
    for (uint32_t target = 0; target < cfg_.scenario.total_conns; ++target) {
      ConnState *conn = roster_[target];
      bk_header header = {};
      header.seq = slot.seq;
      header.sched_ts_ns = slot.sched_ts_ns;
      header.flags = static_cast<uint8_t>(slot.flags & ~BK_FLAG_BROADCAST);
      header.origin_id = cfg_.scenario.total_conns;
      header.traffic_id = slot.traffic_id;
      const size_t frame_size =
          must_deliver ? sizeof(uint32_t) + payload_size : payload_size;
      SendBuf *buf = rudp_bench_msquic::send_buf_new(frame_size, 1);
      bool submitted = false;
      if (buf != nullptr && conn != nullptr &&
          conn->connected.load(std::memory_order_acquire)) {
        uint8_t *payload = buf->data();
        if (must_deliver) {
          const uint32_t nlen = htonl(static_cast<uint32_t>(payload_size));
          std::memcpy(payload, &nlen, sizeof(nlen));
          payload += sizeof(nlen);
        }
        if (bk_authoritative_state_write_applied_input_seq(
                payload, payload_size,
                conn->applied_input_seq.load(std::memory_order_acquire)) == 0 &&
            bk_authoritative_state_fill_target_pad(payload, payload_size,
                                                    target) == 0) {
          header.send_ts_ns = bk_now_ns();
          if (bk_payload_write(payload, payload_size, &header) == 0) {
            if (must_deliver) {
              std::lock_guard<std::mutex> lock(conn->stream_mu);
              const HQUIC stream =
                  conn->stream.load(std::memory_order_acquire);
              submitted = stream != nullptr &&
                          rudp_bench_msquic::send_stream_buf(stream, buf) == 0;
              if (stream == nullptr) {
                rudp_bench_msquic::send_buf_unref(buf);
              }
            } else {
              submitted =
                  rudp_bench_msquic::send_datagram_buf(conn->conn, buf) == 0;
            }
            buf = nullptr;
          }
        }
      }
      if (buf != nullptr) {
        rudp_bench_msquic::send_buf_unref(buf);
      }
      std::lock_guard<std::mutex> lock(metrics_mu_);
      bk_metrics_on_slot(metrics_, &header, submitted);
    }
  }

  void mark_state_unsent(bk_plan *plan, uint64_t cutoff_ns) {
    bk_slot slot;
    std::lock_guard<std::mutex> lock(metrics_mu_);
    while (bk_plan_next(plan, cutoff_ns, &slot)) {
      if ((slot.flags & (BK_FLAG_MEASURE | BK_FLAG_MUST_DELIVER)) ==
          BK_FLAG_MEASURE) {
        stats_.server_state_ticks.fetch_add(1, std::memory_order_relaxed);
      }
      for (uint32_t target = 0; target < cfg_.scenario.total_conns; ++target) {
        (void)target;
        bk_header header = {};
        header.seq = slot.seq;
        header.sched_ts_ns = slot.sched_ts_ns;
        header.flags =
            static_cast<uint8_t>(slot.flags & ~BK_FLAG_BROADCAST);
        header.origin_id = cfg_.scenario.total_conns;
        header.traffic_id = slot.traffic_id;
        bk_metrics_on_slot(metrics_, &header, false);
      }
    }
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
                       event->DATAGRAM_RECEIVED.Buffer->Length, false);
        break;

      case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED:
        rudp_bench_msquic::handle_datagram_send_state(event);
        break;

      case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        conn->connected.store(false, std::memory_order_release);
        {
          std::lock_guard<std::mutex> lock(conn->stream_mu);
          conn->stream.store(nullptr, std::memory_order_release);
        }
        unregister_origin_before_freeze(conn);
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
          if (!ctx->decoder.append(
                  event->RECEIVE.Buffers, event->RECEIVE.BufferCount,
                  [&](const uint8_t *data, size_t len) {
                    ctx->conn->app->handle_payload(ctx->conn, data, len, true);
                  })) {
            stats_.invalid_payload.fetch_add(1, std::memory_order_relaxed);
          }
        }
        break;

      case QUIC_STREAM_EVENT_SEND_COMPLETE:
        rudp_bench_msquic::handle_stream_send_complete(event);
        break;

      case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        {
          std::lock_guard<std::mutex> lock(ctx->conn->stream_mu);
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

  void handle_payload(ConnState *origin, const uint8_t *data, size_t len,
                      bool reliable_path) {
    bk_header header;
    if (bk_payload_read(data, len, &header) != 0) {
      stats_.invalid_payload.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const bool registration = bk_payload_is_registration(&header, len) != 0;
    if (registration) {
      if ((cfg_.scenario.present &&
           header.origin_id >= cfg_.scenario.total_conns) ||
          (cfg_.scenario.kind == BK_SCENARIO_AUTHORITATIVE_STATE &&
           !register_origin(origin, header.origin_id))) {
        stats_.invalid_payload.fetch_add(1, std::memory_order_relaxed);
      }
      return;
    }
    if (reliable_path != ((header.flags & BK_FLAG_MUST_DELIVER) != 0)) {
      stats_.invalid_payload.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if ((cfg_.scenario.present &&
         !bk_scenario_client_payload_valid(&cfg_.scenario, &header, len)) ||
        bk_payload_validate_body(data, len, &header) != 0) {
      stats_.invalid_payload.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (cfg_.scenario.kind == BK_SCENARIO_AUTHORITATIVE_STATE &&
        !register_origin(origin, header.origin_id)) {
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

    if (cfg_.scenario.kind == BK_SCENARIO_AUTHORITATIVE_STATE) {
      if ((header.flags & BK_FLAG_MUST_DELIVER) == 0) {
        uint64_t previous =
            origin->applied_input_seq.load(std::memory_order_relaxed);
        while (header.seq > previous &&
               !origin->applied_input_seq.compare_exchange_weak(
                   previous, header.seq, std::memory_order_release,
                   std::memory_order_relaxed)) {
        }
      }
      std::lock_guard<std::mutex> lock(metrics_mu_);
      bk_metrics_on_recv(metrics_, header.origin_id, &header, bk_now_ns());
      return;
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
        std::lock_guard<std::mutex> lock(target->stream_mu);
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
      uint64_t server_state_ticks;
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
    s.server_state_ticks =
        stats_.server_state_ticks.load(std::memory_order_relaxed);
    bk_authoritative_progress progress = {};
    if (cfg_.scenario.kind == BK_SCENARIO_AUTHORITATIVE_STATE) {
      progress.roster_conns =
          registered_count_.load(std::memory_order_relaxed);
      progress.server_state_ticks = s.server_state_ticks;
    }
    char progress_json[768];
    if (bk_authoritative_progress_format("server", &progress, progress_json,
                                         sizeof(progress_json)) != 0) {
      return -1;
    }
    const int prefix = std::snprintf(
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
        ",\"broadcast\":%" PRIu64 "}},\"invalid_payload\":%" PRIu64 ",",
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
    if (prefix <= 0 || static_cast<size_t>(prefix) >= cap) {
      return -1;
    }
    const int suffix = std::snprintf(
        buf + prefix, cap - static_cast<size_t>(prefix), "%s}", progress_json);
    return suffix > 0 &&
                   static_cast<size_t>(suffix) <
                       cap - static_cast<size_t>(prefix)
               ? 0
               : -1;
  }

 private:
  void unregister_origin_before_freeze(ConnState *conn) {
    if (roster_frozen_.load(std::memory_order_acquire)) {
      return;
    }
    std::lock_guard<std::mutex> lock(roster_mu_);
    if (roster_frozen_.load(std::memory_order_relaxed)) {
      return;
    }
    const int64_t origin_id =
        conn->origin_id.load(std::memory_order_relaxed);
    if (origin_id >= 0 && origin_id < static_cast<int64_t>(roster_.size()) &&
        roster_[static_cast<size_t>(origin_id)] == conn) {
      roster_[static_cast<size_t>(origin_id)] = nullptr;
      conn->origin_id.store(-1, std::memory_order_release);
      conn->applied_input_seq.store(0, std::memory_order_relaxed);
      registered_count_.fetch_sub(1, std::memory_order_release);
    }
  }

  bool register_origin(ConnState *conn, uint32_t origin_id) {
    int64_t existing = conn->origin_id.load(std::memory_order_acquire);
    if (existing >= 0) {
      return existing == static_cast<int64_t>(origin_id);
    }
    std::lock_guard<std::mutex> lock(roster_mu_);
    existing = conn->origin_id.load(std::memory_order_relaxed);
    if (existing >= 0) {
      return existing == static_cast<int64_t>(origin_id);
    }
    if (roster_frozen_.load(std::memory_order_acquire) ||
        roster_[origin_id] != nullptr) {
      return false;
    }
    roster_[origin_id] = conn;
    conn->origin_id.store(static_cast<int64_t>(origin_id),
                          std::memory_order_release);
    registered_count_.fetch_add(1, std::memory_order_release);
    return true;
  }

  void accept_peer_stream(ConnState *conn, HQUIC stream) {
    auto *ctx = new StreamCtx();
    ctx->conn = conn;
    ctx->stream = stream;
    MsQuic->SetCallbackHandler(stream, reinterpret_cast<void *>(stream_cb),
                               ctx);
    std::lock_guard<std::mutex> lock(conn->stream_mu);
    HQUIC expected = nullptr;
    conn->stream.compare_exchange_strong(expected, stream);
  }

  int send_by_flags(ConnState *conn, const void *data, size_t len,
                    uint8_t flags) {
    if ((flags & BK_FLAG_MUST_DELIVER) == 0) {
      return rudp_bench_msquic::send_datagram_payload(conn->conn, data, len);
    }
    std::lock_guard<std::mutex> lock(conn->stream_mu);
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
  ServerConfig cfg_;
  bk_metrics *metrics_ = nullptr;
  std::mutex metrics_mu_;
  std::mutex roster_mu_;
  std::vector<ConnState *> roster_;
  std::atomic<uint32_t> registered_count_{0};
  std::atomic<bool> roster_frozen_{false};
};

}  // namespace

int main(int argc, char **argv) {
  ServerConfig cfg;
  if (parse_args(argc, argv, &cfg) != 0) {
    usage(argv[0]);
    return EXIT_FAILURE;
  }

  if (std::signal(SIGINT, on_signal) == SIG_ERR ||
      std::signal(SIGTERM, on_signal) == SIG_ERR) {
    std::perror("signal");
    return EXIT_FAILURE;
  }

  auto *app = new ServerApp(cfg);
  if (!app->start(cfg.port)) {
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

  const bool authoritative =
      cfg.scenario.kind == BK_SCENARIO_AUTHORITATIVE_STATE;
  bool schedule_valid = control != nullptr;
  bool window_final = false;
  bool roster_frozen = false;
  bool state_marked_unsent = false;
  int run_rc = 0;
  bk_plan *state_plan = nullptr;
  while (!g_stop) {
    uint64_t now = bk_now_ns();
    if (authoritative && !schedule_valid && app->roster_complete()) {
      schedule.start_at_ns =
          rudp_bench_msquic::add_ns(now, kDevWarmupNs);
      schedule.stop_at_ns =
          rudp_bench_msquic::add_ns(schedule.start_at_ns, kDevDurationNs);
      schedule.drain_until_ns =
          rudp_bench_msquic::add_ns(schedule.stop_at_ns, kDevDrainNs);
      schedule_valid = true;
    }
    if (authoritative && schedule_valid && !roster_frozen &&
        now >= schedule.start_at_ns) {
      std::fprintf(stderr,
                   "authoritative roster incomplete before measurement "
                   "start\n");
      run_rc = -1;
      break;
    }
    if (authoritative && schedule_valid && !roster_frozen &&
        app->freeze_roster()) {
      bk_stream streams[2] = {};
      int n_streams = 0;
      if (build_state_streams(cfg, streams, &n_streams) != 0) {
        run_rc = -1;
        break;
      }
      state_plan = bk_plan_new(streams, n_streams, now,
                               schedule.start_at_ns, schedule.stop_at_ns);
      if (state_plan == nullptr || app->expect_client_inputs(schedule) != 0) {
        std::fprintf(stderr, "authoritative state initialization failed\n");
        run_rc = -1;
        break;
      }
      roster_frozen = true;
    }

    if (control != nullptr && !window_final) {
      if (now >= schedule.start_at_ns) {
        window_final = true;
      } else {
        const int wr = bk_control_poll_window(control, &schedule);
        if (wr < 0) {
          std::fprintf(stderr, "benchkit window poll failed\n");
          run_rc = -1;
          break;
        }
        if (wr == 1) {
          window_final = true;
          if (state_plan != nullptr) {
            bk_plan_set_window(state_plan, schedule.start_at_ns,
                               schedule.stop_at_ns);
          }
        }
      }
    }

    now = bk_now_ns();
    if (schedule_valid && now >= schedule.start_at_ns &&
        now < schedule.stop_at_ns) {
      app->metrics_tick(now);
    }
    if (state_plan != nullptr && now < schedule.stop_at_ns) {
      bk_slot slot;
      while (bk_plan_next(state_plan, now, &slot)) {
        app->send_state_slot(slot);
      }
    } else if (state_plan != nullptr && !state_marked_unsent) {
      const uint64_t cutoff =
          schedule.stop_at_ns == 0 ? 0 : schedule.stop_at_ns - 1u;
      app->mark_state_unsent(state_plan, cutoff);
      state_marked_unsent = true;
    }
    if (control != nullptr && now >= schedule.drain_until_ns) {
      break;
    }

    uint64_t sleep_ns = static_cast<uint64_t>(kServiceSliceMs) * 1000000ull;
    if (state_plan != nullptr && now < schedule.stop_at_ns) {
      const uint64_t due = bk_plan_peek_ns(state_plan);
      if (due <= now) {
        sleep_ns = 0;
      } else if (due - now < sleep_ns) {
        sleep_ns = due - now;
      }
    }
    if (sleep_ns == 0) {
      std::this_thread::yield();
    } else {
      std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
    }
  }

  if (state_plan != nullptr && !state_marked_unsent) {
    const uint64_t cutoff =
        schedule.stop_at_ns == 0 ? 0 : schedule.stop_at_ns - 1u;
    app->mark_state_unsent(state_plan, cutoff);
  }

  char stats_json[4096];
  if (app->format_stats_json(stats_json, sizeof(stats_json)) != 0) {
    std::fprintf(stderr, "server stats JSON overflow\n");
    if (control != nullptr) {
      bk_control_close(control);
    }
    return EXIT_FAILURE;
  }

  if (app->dump_metrics_out() != 0) {
    std::fprintf(stderr, "failed to write BENCH_METRICS_OUT\n");
    run_rc = -1;
  }

  if (control != nullptr && run_rc == 0) {
    if (bk_control_done(control, stats_json) != 0) {
      std::fprintf(stderr, "benchkit done failed\n");
      run_rc = -1;
    }
    bk_control_close(control);
    control = nullptr;
  }
  if (control != nullptr) {
    bk_control_close(control);
  }

  bk_plan_free(state_plan);

  return run_rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
