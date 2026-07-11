#include "benchkit.h"
#include "../scenario_cli.h"
#include "msquic_common.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

using rudp_bench_msquic::FrameDecoder;
using rudp_bench_msquic::MsQuic;

constexpr uint64_t kDevWarmupNs = 200000000ull;
constexpr uint64_t kDevDurationNs = 2000000000ull;
constexpr uint64_t kDevDrainNs = 500000000ull;
constexpr uint64_t kConnectTimeoutNs = 10000000000ull;
constexpr uint64_t kServiceMaxSleepNs = 10000000ull;
constexpr int kMaxConns = 4095;

struct ClientConfig {
  const char *host = nullptr;
  uint16_t port = 0;
  int conns = 0;
  int proc_index = 0;
  uint32_t origin_base = 0;
  double rate_lt = 0.0;
  double rate_md = 0.0;
  bool broadcast_lt = false;
  bool broadcast_md = false;
  size_t payload_lt = 0;  // class 別 payload(0 = 未指定)
  size_t payload_md = 0;
  uint64_t deadline_ns = 0;
  uint64_t staleness_period_ns = 0;
  bk_scenario_cli scenario = {};
  uint8_t traffic_id = 0;
  bk_direction direction = BK_DIRECTION_ROOM_RELAY;
};

class ClientApp;

struct ClientConn {
  ClientApp *app = nullptr;
  HQUIC conn = nullptr;
  HQUIC stream = nullptr;
  uint32_t origin_id = 0;
  uint32_t local_index = 0;  // 自 proc 内 0 起点(重複判定の受信側キー)
  bool connected = false;
  bool stream_ready = false;
  bk_plan *plan = nullptr;
  FrameDecoder decoder;
  std::mutex mu;
  std::mutex decoder_mu;
  std::atomic<uint64_t> last_input_seq{0};
  std::atomic<uint64_t> last_applied_input_seq{0};
  std::atomic<uint64_t> input_last_sent_measured{0};
  std::atomic<uint64_t> state_header_seq_recv_measured{0};
  std::atomic<uint64_t> state_applied_input_seq_recv_measured{0};
};

void usage(const char *argv0) {
  std::fprintf(stderr,
               "usage: %s --host HOST --port PORT --conns N --proc-index N "
               "--origin-base N --rate-lt HZ --rate-md HZ --payload BYTES "
               "[--payload-lt BYTES] [--payload-md BYTES] "
               "--deadline-ns NS --staleness-period-ns NS\n",
               argv0);
}

int parse_u64(const char *s, uint64_t *out) {
  if (s == nullptr || *s == '\0') {
    return -1;
  }
  errno = 0;
  char *end = nullptr;
  const unsigned long long v = std::strtoull(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0') {
    return -1;
  }
  *out = static_cast<uint64_t>(v);
  return 0;
}

int parse_u32(const char *s, uint32_t *out) {
  uint64_t v = 0;
  if (parse_u64(s, &v) != 0 || v > UINT32_MAX) {
    return -1;
  }
  *out = static_cast<uint32_t>(v);
  return 0;
}

int parse_i32(const char *s, int *out) {
  if (s == nullptr || *s == '\0') {
    return -1;
  }
  errno = 0;
  char *end = nullptr;
  const long v = std::strtol(s, &end, 10);
  if (errno != 0 || end == s || *end != '\0' || v < INT_MIN || v > INT_MAX) {
    return -1;
  }
  *out = static_cast<int>(v);
  return 0;
}

int parse_u16(const char *s, uint16_t *out) {
  uint64_t v = 0;
  if (parse_u64(s, &v) != 0 || v > UINT16_MAX) {
    return -1;
  }
  *out = static_cast<uint16_t>(v);
  return 0;
}

int parse_size(const char *s, size_t *out) {
  uint64_t v = 0;
  if (parse_u64(s, &v) != 0 || v > static_cast<uint64_t>(SIZE_MAX)) {
    return -1;
  }
  *out = static_cast<size_t>(v);
  return 0;
}

int parse_rate(const char *s, double *out) {
  if (s == nullptr || *s == '\0') {
    return -1;
  }
  errno = 0;
  char *end = nullptr;
  const double v = std::strtod(s, &end);
  if (errno != 0 || end == s || *end != '\0' || v < 0.0) {
    return -1;
  }
  *out = v;
  return 0;
}

int parse_args(int argc, char **argv, ClientConfig *cfg) {
  cfg->staleness_period_ns = 10000000ull;
  bool have_host = false;
  bool have_port = false;
  bool have_conns = false;
  bool have_proc_index = false;
  bool have_origin_base = false;
  bool have_rate_lt = false;
  bool have_rate_md = false;
  bool have_payload = false;
  bool have_deadline = false;
  bool have_staleness = false;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--describe") == 0) {
      rudp_bench_msquic::print_describe();
      std::exit(EXIT_SUCCESS);
    }
    const int scenario_arg =
        bk_scenario_cli_parse(argc, argv, &i, &cfg->scenario);
    if (scenario_arg < 0) {
      return -1;
    }
    if (scenario_arg > 0) {
      continue;
    }
    if (std::strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
      cfg->host = argv[++i];
      have_host = cfg->host[0] != '\0';
      continue;
    }
    if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      have_port = parse_u16(argv[++i], &cfg->port) == 0 && cfg->port != 0;
      continue;
    }
    if (std::strcmp(argv[i], "--conns") == 0 && i + 1 < argc) {
      have_conns = parse_i32(argv[++i], &cfg->conns) == 0 &&
                   cfg->conns > 0 && cfg->conns <= kMaxConns;
      continue;
    }
    if (std::strcmp(argv[i], "--proc-index") == 0 && i + 1 < argc) {
      have_proc_index =
          parse_i32(argv[++i], &cfg->proc_index) == 0 && cfg->proc_index >= 0;
      continue;
    }
    if (std::strcmp(argv[i], "--origin-base") == 0 && i + 1 < argc) {
      have_origin_base = parse_u32(argv[++i], &cfg->origin_base) == 0;
      continue;
    }
    if (std::strcmp(argv[i], "--rate-lt") == 0 && i + 1 < argc) {
      have_rate_lt = parse_rate(argv[++i], &cfg->rate_lt) == 0;
      continue;
    }
    if (std::strcmp(argv[i], "--rate-md") == 0 && i + 1 < argc) {
      have_rate_md = parse_rate(argv[++i], &cfg->rate_md) == 0;
      continue;
    }
    if (std::strcmp(argv[i], "--broadcast-lt") == 0) {
      cfg->broadcast_lt = true;
      continue;
    }
    if (std::strcmp(argv[i], "--broadcast-md") == 0) {
      cfg->broadcast_md = true;
      continue;
    }
    if (std::strcmp(argv[i], "--payload") == 0 && i + 1 < argc) {
      size_t v = 0;
      have_payload = parse_size(argv[++i], &v) == 0;
      cfg->payload_lt = v;
      cfg->payload_md = v;
      continue;
    }
    if (std::strcmp(argv[i], "--payload-lt") == 0 && i + 1 < argc) {
      have_payload = parse_size(argv[++i], &cfg->payload_lt) == 0;
      continue;
    }
    if (std::strcmp(argv[i], "--payload-md") == 0 && i + 1 < argc) {
      have_payload = parse_size(argv[++i], &cfg->payload_md) == 0;
      continue;
    }
    if (std::strcmp(argv[i], "--deadline-ns") == 0 && i + 1 < argc) {
      have_deadline = parse_u64(argv[++i], &cfg->deadline_ns) == 0;
      continue;
    }
    if (std::strcmp(argv[i], "--staleness-period-ns") == 0 &&
        i + 1 < argc) {
      have_staleness =
          parse_u64(argv[++i], &cfg->staleness_period_ns) == 0 &&
          cfg->staleness_period_ns > 0;
      continue;
    }
    return -1;
  }

  if (!have_host || !have_port || !have_conns || !have_proc_index ||
      !have_origin_base) {
    return -1;
  }
  if (cfg->scenario.present) {
    if (bk_scenario_cli_validate(&cfg->scenario, kMaxConns,
                                 rudp_bench_msquic::kMaxPayloadBytes) != 0 ||
        static_cast<uint64_t>(cfg->origin_base) +
                static_cast<uint64_t>(cfg->conns) >
            cfg->scenario.total_conns) {
      return -1;
    }
    int broadcast = 0;
    const bk_scenario_traffic *traffic = bk_scenario_client_traffic(
        &cfg->scenario, &cfg->direction, &broadcast);
    if (traffic == nullptr) {
      return -1;
    }
    cfg->traffic_id = traffic->traffic_id;
    cfg->rate_lt = traffic->rate_lt;
    cfg->rate_md = traffic->rate_md;
    cfg->payload_lt = traffic->payload_lt;
    cfg->payload_md = traffic->payload_md;
    cfg->deadline_ns = traffic->deadline_ns;
    cfg->broadcast_lt = broadcast != 0;
    cfg->broadcast_md = broadcast != 0;
    return 0;
  }
  if (!have_rate_lt || !have_rate_md || !have_payload || !have_deadline ||
      !have_staleness) {
    return -1;
  }
  if (cfg->rate_lt == 0.0 && cfg->rate_md == 0.0) {
    return -1;
  }
  // 有効な stream の payload が範囲内であること
  if (cfg->rate_lt > 0.0 &&
      (cfg->payload_lt < BK_MIN_PAYLOAD ||
       cfg->payload_lt > rudp_bench_msquic::kMaxPayloadBytes)) {
    return -1;
  }
  if (cfg->rate_md > 0.0 &&
      (cfg->payload_md < BK_MIN_PAYLOAD ||
       cfg->payload_md > rudp_bench_msquic::kMaxPayloadBytes)) {
    return -1;
  }
  if (static_cast<uint64_t>(cfg->origin_base) +
          static_cast<uint64_t>(cfg->conns) >
      UINT32_MAX) {
    return -1;
  }
  return 0;
}

int interval_from_rate(double rate_hz, uint64_t *out) {
  if (rate_hz <= 0.0) {
    return -1;
  }
  const double interval = 1000000000.0 / rate_hz;
  if (interval < 1.0 || interval > static_cast<double>(UINT64_MAX)) {
    return -1;
  }
  uint64_t ns = static_cast<uint64_t>(interval + 0.5);
  if (ns == 0) {
    ns = 1;
  }
  *out = ns;
  return 0;
}

int build_streams(const ClientConfig *cfg, bk_stream *streams,
                  int *n_streams) {
  int n = 0;
  uint64_t interval_ns = 0;
  if (cfg->rate_lt > 0.0) {
    if (interval_from_rate(cfg->rate_lt, &interval_ns) != 0) {
      return -1;
    }
    streams[n].must_deliver = false;
    streams[n].broadcast = cfg->broadcast_lt;
    streams[n].traffic_id = cfg->traffic_id;
    streams[n].direction = cfg->direction;
    streams[n].interval_ns = interval_ns;
    ++n;
  }
  if (cfg->rate_md > 0.0) {
    if (interval_from_rate(cfg->rate_md, &interval_ns) != 0) {
      return -1;
    }
    streams[n].must_deliver = true;
    streams[n].broadcast = cfg->broadcast_md;
    streams[n].traffic_id = cfg->traffic_id;
    streams[n].direction = cfg->direction;
    streams[n].interval_ns = interval_ns;
    ++n;
  }
  *n_streams = n;
  return n > 0 ? 0 : -1;
}

void make_header_from_slot(const bk_slot *slot, uint32_t origin_id,
                           uint64_t send_ts_ns, bk_header *header) {
  header->seq = slot->seq;
  header->sched_ts_ns = slot->sched_ts_ns;
  header->send_ts_ns = send_ts_ns;
  header->flags = slot->flags;
  header->origin_id = origin_id;
  header->traffic_id = slot->traffic_id;
}

const char *metrics_path_or_default(char *buf, size_t cap) {
  const char *path = std::getenv("BENCH_METRICS_OUT");
  if (path != nullptr && *path != '\0') {
    return path;
  }
  const int n = std::snprintf(buf, cap, "/tmp/rudp-bench-msquic-client-%ld.json",
                              static_cast<long>(::getpid()));
  if (n <= 0 || static_cast<size_t>(n) >= cap) {
    return nullptr;
  }
  return buf;
}

uint64_t next_plan_due(const std::vector<ClientConn *> &conns) {
  uint64_t next = UINT64_MAX;
  for (const ClientConn *conn : conns) {
    const uint64_t due = bk_plan_peek_ns(conn->plan);
    if (due < next) {
      next = due;
    }
  }
  return next;
}

uint64_t sleep_ns_for_next(uint64_t now_ns, uint64_t next_ns,
                           uint64_t limit_ns) {
  uint64_t wake_ns = next_ns;
  const uint64_t slice_ns =
      rudp_bench_msquic::add_ns(now_ns, kServiceMaxSleepNs);
  if (wake_ns > slice_ns) {
    wake_ns = slice_ns;
  }
  if (wake_ns > limit_ns) {
    wake_ns = limit_ns;
  }
  if (wake_ns <= now_ns) {
    return 0;
  }
  return wake_ns - now_ns;
}

class ClientApp {
 public:
  explicit ClientApp(const ClientConfig &cfg) : cfg_(cfg) {}

  int run() {
    const bool authoritative =
        cfg_.scenario.kind == BK_SCENARIO_AUTHORITATIVE_STATE;
    const uint32_t max_origin_id =
        cfg_.scenario.present
            ? cfg_.scenario.total_conns + 1u
            : cfg_.origin_base + static_cast<uint32_t>(cfg_.conns);
    bk_metrics_config metrics_cfg = {};
    metrics_cfg.max_origin_id = max_origin_id == 0 ? 1u : max_origin_id;
    metrics_cfg.deadline_ns = cfg_.deadline_ns;
    metrics_cfg.staleness_period_ns = cfg_.staleness_period_ns;
    metrics_cfg.max_local_index =
        cfg_.scenario.present ? static_cast<uint32_t>(cfg_.conns) : 0;
    metrics_ = bk_metrics_new(&metrics_cfg);
    if (metrics_ == nullptr) {
      std::fprintf(stderr, "bk_metrics_new failed\n");
      return -1;
    }
    if (authoritative &&
        bk_metrics_set_traffic_deadline(
            metrics_, cfg_.scenario.state.traffic_id,
            BK_DIRECTION_SERVER_TO_CLIENT,
            cfg_.scenario.state.deadline_ns) != 0) {
      return -1;
    }

    bk_control *control = bk_control_connect(nullptr);
    if (control != nullptr &&
        bk_control_hello(control, "client", "msquic", cfg_.proc_index) != 0) {
      std::fprintf(stderr, "benchkit hello failed\n");
      bk_control_close(control);
      return -1;
    }

    if (!open_quic()) {
      if (control != nullptr) {
        bk_control_close(control);
      }
      return -1;
    }
    if (!open_connections()) {
      if (control != nullptr) {
        bk_control_close(control);
      }
      return -1;
    }
    if (!wait_for_ready()) {
      std::fprintf(stderr, "timed out waiting for MsQuic connections\n");
      if (control != nullptr) {
        bk_control_close(control);
      }
      return -1;
    }
    if (!send_registrations()) {
      std::fprintf(stderr, "MsQuic registration send failed\n");
      if (control != nullptr) {
        bk_control_close(control);
      }
      return -1;
    }

    bk_schedule schedule;
    if (control != nullptr) {
      if (bk_control_ready(control, cfg_.conns) != 0 ||
          bk_control_wait_schedule(control, &schedule) != 0) {
        std::fprintf(stderr, "benchkit schedule failed\n");
        bk_control_close(control);
        return -1;
      }
    } else {
      const uint64_t now = bk_now_ns();
      schedule.start_at_ns = rudp_bench_msquic::add_ns(now, kDevWarmupNs);
      schedule.stop_at_ns =
          rudp_bench_msquic::add_ns(schedule.start_at_ns, kDevDurationNs);
      schedule.drain_until_ns =
          rudp_bench_msquic::add_ns(schedule.stop_at_ns, kDevDrainNs);
    }

    bk_stream streams[2];
    int n_streams = 0;
    if (build_streams(&cfg_, streams, &n_streams) != 0) {
      std::fprintf(stderr, "invalid rates\n");
      if (control != nullptr) {
        bk_control_close(control);
      }
      return -1;
    }

    const uint64_t plan_start_ns = bk_now_ns();
    for (ClientConn *conn : conns_) {
      conn->plan =
          bk_plan_new(streams, n_streams, plan_start_ns, schedule.start_at_ns,
                      schedule.stop_at_ns);
      if (conn->plan == nullptr) {
        std::fprintf(stderr, "bk_plan_new failed\n");
        if (control != nullptr) {
          bk_control_close(control);
        }
        return -1;
      }
    }
    if (!expect_scenario_flows(schedule)) {
      std::fprintf(stderr, "bk_metrics_expect_latest failed\n");
      if (control != nullptr) {
        bk_control_close(control);
      }
      return -1;
    }

    int rc = run_schedule(control, schedule);

    char default_metrics_path[128];
    const char *metrics_path =
        metrics_path_or_default(default_metrics_path, sizeof(default_metrics_path));
    if (metrics_path == nullptr || dump_metrics(metrics_path) != 0) {
      std::fprintf(stderr, "bk_metrics_dump_json failed\n");
      rc = -1;
    }

    char stats_json[1024];
    if (format_stats_json(stats_json, sizeof(stats_json)) != 0) {
      std::fprintf(stderr, "client stats JSON overflow\n");
      rc = -1;
    }
    if (control != nullptr) {
      if (rc == 0 && bk_control_done(control, stats_json) != 0) {
        std::fprintf(stderr, "benchkit done failed\n");
        rc = -1;
      }
      bk_control_close(control);
    }

    for (ClientConn *conn : conns_) {
      if (conn->plan != nullptr) {
        bk_plan_free(conn->plan);
        conn->plan = nullptr;
      }
    }
    return rc;
  }

  QUIC_STATUS on_connection_event(ClientConn *conn,
                                  QUIC_CONNECTION_EVENT *event) {
    switch (event->Type) {
      case QUIC_CONNECTION_EVENT_CONNECTED:
        on_connected(conn);
        break;

      case QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED:
        record_recv(conn->local_index, event->DATAGRAM_RECEIVED.Buffer->Buffer,
                    event->DATAGRAM_RECEIVED.Buffer->Length, false);
        break;

      case QUIC_CONNECTION_EVENT_DATAGRAM_SEND_STATE_CHANGED:
        rudp_bench_msquic::handle_datagram_send_state(event);
        break;

      case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_TRANSPORT:
      case QUIC_CONNECTION_EVENT_SHUTDOWN_INITIATED_BY_PEER:
        failed_.store(true, std::memory_order_relaxed);
        break;

      case QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE:
        {
          std::lock_guard<std::mutex> lock(conn->mu);
          conn->connected = false;
          conn->stream = nullptr;
        }
        break;

      default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
  }

  QUIC_STATUS on_stream_event(ClientConn *conn, HQUIC stream,
                              QUIC_STREAM_EVENT *event) {
    switch (event->Type) {
      case QUIC_STREAM_EVENT_START_COMPLETE:
        if (QUIC_FAILED(event->START_COMPLETE.Status)) {
          failed_.store(true, std::memory_order_relaxed);
          break;
        }
        {
          std::lock_guard<std::mutex> lock(conn->mu);
          if (!conn->stream_ready) {
            conn->stream_ready = true;
            stream_ready_count_.fetch_add(1, std::memory_order_relaxed);
          }
        }
        break;

      case QUIC_STREAM_EVENT_RECEIVE:
        {
          std::lock_guard<std::mutex> lock(conn->decoder_mu);
          if (!conn->decoder.append(
                  event->RECEIVE.Buffers, event->RECEIVE.BufferCount,
                  [&](const uint8_t *data, size_t len) {
                    record_recv(conn->local_index, data, len, true);
                  })) {
            invalid_payload_.fetch_add(1, std::memory_order_relaxed);
          }
        }
        break;

      case QUIC_STREAM_EVENT_SEND_COMPLETE:
        rudp_bench_msquic::handle_stream_send_complete(event);
        break;

      case QUIC_STREAM_EVENT_SHUTDOWN_COMPLETE:
        {
          std::lock_guard<std::mutex> lock(conn->mu);
          if (conn->stream == stream) {
            conn->stream = nullptr;
          }
        }
        MsQuic->StreamClose(stream);
        break;

      default:
        break;
    }
    return QUIC_STATUS_SUCCESS;
  }

 private:
  bool open_quic() {
    rudp_bench_msquic::ensure_msquic();

    QUIC_REGISTRATION_CONFIG reg_cfg = {"rudp-bench-v2-msquic-client",
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

    QUIC_CREDENTIAL_CONFIG cred_cfg = {};
    cred_cfg.Type = QUIC_CREDENTIAL_TYPE_NONE;
    cred_cfg.Flags = QUIC_CREDENTIAL_FLAG_CLIENT |
                     QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION;
    return rudp_bench_msquic::status_ok(
        MsQuic->ConfigurationLoadCredential(configuration_, &cred_cfg),
        "ConfigurationLoadCredential");
  }

  bool open_connections() {
    conns_.reserve(static_cast<size_t>(cfg_.conns));
    for (int i = 0; i < cfg_.conns; ++i) {
      auto *conn = new ClientConn();
      conn->app = this;
      conn->origin_id = cfg_.origin_base + static_cast<uint32_t>(i);
      conn->local_index = static_cast<uint32_t>(i);
      if (!rudp_bench_msquic::status_ok(
              MsQuic->ConnectionOpen(registration_, connection_cb, conn,
                                     &conn->conn),
              "ConnectionOpen")) {
        return false;
      }
      conns_.push_back(conn);
      if (!rudp_bench_msquic::status_ok(
              MsQuic->ConnectionStart(conn->conn, configuration_,
                                      QUIC_ADDRESS_FAMILY_UNSPEC, cfg_.host,
                                      cfg_.port),
              "ConnectionStart")) {
        return false;
      }
    }
    return true;
  }

  void on_connected(ClientConn *conn) {
    {
      std::lock_guard<std::mutex> lock(conn->mu);
      if (conn->connected) {
        return;
      }
      conn->connected = true;
      connected_count_.fetch_add(1, std::memory_order_relaxed);
    }
    HQUIC stream = nullptr;
    if (!rudp_bench_msquic::status_ok(
            MsQuic->StreamOpen(conn->conn, QUIC_STREAM_OPEN_FLAG_NONE,
                               stream_cb, conn, &stream),
            "StreamOpen")) {
      failed_.store(true, std::memory_order_relaxed);
      return;
    }
    {
      std::lock_guard<std::mutex> lock(conn->mu);
      conn->stream = stream;
    }
    if (!rudp_bench_msquic::status_ok(
            MsQuic->StreamStart(stream, QUIC_STREAM_START_FLAG_IMMEDIATE),
            "StreamStart")) {
      failed_.store(true, std::memory_order_relaxed);
    }
  }

  bool wait_for_ready() {
    const uint64_t deadline =
        rudp_bench_msquic::add_ns(bk_now_ns(), kConnectTimeoutNs);
    while (stream_ready_count_.load(std::memory_order_relaxed) < cfg_.conns) {
      if (failed_.load(std::memory_order_relaxed) || bk_now_ns() >= deadline) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return connected_count_.load(std::memory_order_relaxed) == cfg_.conns;
  }

  bool send_registrations() {
    for (ClientConn *conn : conns_) {
      const size_t payload_size = BK_MIN_PAYLOAD;
      rudp_bench_msquic::SendBuf *buf = rudp_bench_msquic::send_buf_new(
          sizeof(uint32_t) + payload_size, 1);
      if (buf == nullptr) {
        return false;
      }
      const uint32_t nlen = htonl(static_cast<uint32_t>(payload_size));
      std::memcpy(buf->data(), &nlen, sizeof(nlen));
      bk_header header = {};
      header.send_ts_ns = bk_now_ns();
      header.origin_id = conn->origin_id;
      if (bk_payload_write(buf->data() + sizeof(nlen), payload_size, &header) !=
          0) {
        rudp_bench_msquic::send_buf_unref(buf);
        return false;
      }
      std::lock_guard<std::mutex> lock(conn->mu);
      const HQUIC stream = conn->stream_ready ? conn->stream : nullptr;
      if (stream == nullptr) {
        rudp_bench_msquic::send_buf_unref(buf);
        return false;
      }
      if (rudp_bench_msquic::send_stream_buf(stream, buf) != 0) {
        return false;
      }
    }
    return true;
  }

  bool expect_scenario_flows(const bk_schedule &schedule) {
    if (!cfg_.scenario.present) {
      return true;
    }
    std::lock_guard<std::mutex> lock(metrics_mu_);
    if (cfg_.scenario.kind == BK_SCENARIO_AUTHORITATIVE_STATE) {
      if (cfg_.scenario.state.rate_lt == 0.0) {
        return true;
      }
      for (const ClientConn *conn : conns_) {
        if (bk_metrics_expect_latest(
                metrics_, conn->local_index, cfg_.scenario.total_conns,
                cfg_.scenario.state.traffic_id,
                BK_DIRECTION_SERVER_TO_CLIENT, schedule.start_at_ns) != 0) {
          return false;
        }
      }
      return true;
    }
    if (cfg_.rate_lt == 0.0) {
      return true;
    }
    for (const ClientConn *conn : conns_) {
      const uint32_t first_origin =
          cfg_.scenario.kind == BK_SCENARIO_ROOM_RELAY ? 0u
                                                       : conn->origin_id;
      const uint32_t end_origin =
          cfg_.scenario.kind == BK_SCENARIO_ROOM_RELAY
              ? cfg_.scenario.total_conns
              : first_origin + 1u;
      for (uint32_t origin = first_origin; origin < end_origin; ++origin) {
        if (bk_metrics_expect_latest(metrics_, conn->local_index, origin,
                                     cfg_.traffic_id,
                                     BK_DIRECTION_ROOM_RELAY,
                                     schedule.start_at_ns) != 0) {
          return false;
        }
      }
    }
    return true;
  }

  int run_schedule(bk_control *control, bk_schedule schedule) {
    bool marked_unsent = false;
    int rc = 0;
    bk_steady steady = {0, false};

    while (bk_now_ns() < schedule.drain_until_ns) {
      uint64_t now = bk_now_ns();
      if (failed_.load(std::memory_order_relaxed)) {
        std::fprintf(stderr, "msquic connection failed during run\n");
        rc = -1;
        break;
      }

      // 定常判定つき warmup(benchspec v2): rate 報告と確定窓(window)の受信。
      // raw counts は metrics_mu_ 下で読む(msquic は callback スレッドが書く)
      if (control != nullptr) {
        uint64_t raw_submitted = 0;
        uint64_t raw_rm = 0;
        uint64_t raw_ru = 0;
        {
          std::lock_guard<std::mutex> lock(metrics_mu_);
          bk_metrics_raw_counts(metrics_, nullptr, &raw_submitted, &raw_rm,
                                &raw_ru);
        }
        const int sr = bk_steady_tick(&steady, control, raw_submitted,
                                      raw_rm + raw_ru, &schedule, now);
        if (sr < 0) {
          std::fprintf(stderr, "benchkit steady tick failed\n");
          rc = -1;
          break;
        }
        if (sr == 1) {
          for (ClientConn *conn : conns_) {
            bk_plan_set_window(conn->plan, schedule.start_at_ns,
                               schedule.stop_at_ns);
          }
        }
      }

      if (now >= schedule.start_at_ns && now < schedule.stop_at_ns) {
        metrics_tick(now);
      }

      if (now < schedule.stop_at_ns) {
        for (ClientConn *conn : conns_) {
          bk_slot slot;
          while (bk_plan_next(conn->plan, now, &slot)) {
            const size_t payload_size = (slot.flags & BK_FLAG_MUST_DELIVER)
                                            ? cfg_.payload_md
                                            : cfg_.payload_lt;
            send_slot(conn, &slot, payload_size);
          }
        }
      } else if (!marked_unsent) {
        const uint64_t cutoff =
            schedule.stop_at_ns == 0 ? 0 : schedule.stop_at_ns - 1u;
        for (ClientConn *conn : conns_) {
          mark_unsent_until(conn, cutoff);
        }
        marked_unsent = true;
      }

      now = bk_now_ns();
      uint64_t next_ns = schedule.drain_until_ns;
      if (now < schedule.stop_at_ns) {
        const uint64_t due = next_plan_due(conns_);
        if (due < next_ns) {
          next_ns = due;
        }
      }
      const uint64_t sleep_ns =
          sleep_ns_for_next(now, next_ns, schedule.drain_until_ns);
      if (sleep_ns == 0) {
        std::this_thread::yield();
      } else {
        std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
      }
    }

    if (!marked_unsent) {
      const uint64_t cutoff =
          schedule.stop_at_ns == 0 ? 0 : schedule.stop_at_ns - 1u;
      for (ClientConn *conn : conns_) {
        mark_unsent_until(conn, cutoff);
      }
    }
    return rc;
  }

  // frame を SendBuf に直接構築し、header と deterministic body pattern を
  // 書く。中間バッファ経由の payload 全体 memcpy を避ける。
  void send_slot(ClientConn *conn, const bk_slot *slot, size_t payload_size) {
    bk_header header;
    make_header_from_slot(slot, conn->origin_id, bk_now_ns(), &header);
    bool submitted = false;
    if ((slot->flags & BK_FLAG_MUST_DELIVER) == 0) {
      rudp_bench_msquic::SendBuf *buf =
          rudp_bench_msquic::send_buf_new(payload_size, 1);
      if (buf != nullptr) {
        if (bk_payload_write(buf->data(), payload_size, &header) == 0 &&
            bk_payload_fill_body(buf->data(), payload_size, &header) == 0) {
          submitted =
              rudp_bench_msquic::send_datagram_buf(conn->conn, buf) == 0;
        } else {
          rudp_bench_msquic::send_buf_unref(buf);
        }
      }
    } else {
      rudp_bench_msquic::SendBuf *buf = rudp_bench_msquic::send_buf_new(
          sizeof(uint32_t) + payload_size, 1);
      if (buf != nullptr) {
        const uint32_t nlen = htonl(static_cast<uint32_t>(payload_size));
        std::memcpy(buf->data(), &nlen, sizeof(nlen));
        if (bk_payload_write(buf->data() + sizeof(nlen), payload_size,
                             &header) == 0 &&
            bk_payload_fill_body(buf->data() + sizeof(nlen), payload_size,
                                 &header) == 0) {
          std::lock_guard<std::mutex> lock(conn->mu);
          const HQUIC stream = conn->stream_ready ? conn->stream : nullptr;
          if (stream != nullptr) {
            submitted = rudp_bench_msquic::send_stream_buf(stream, buf) == 0;
            buf = nullptr;
          }
        }
        if (buf != nullptr) {
          rudp_bench_msquic::send_buf_unref(buf);
        }
      }
    }
    if (submitted && BK_FLAGS_DIRECTION(header.flags) ==
                         BK_DIRECTION_CLIENT_TO_SERVER &&
        (header.flags & BK_FLAG_MUST_DELIVER) == 0) {
      uint64_t previous = conn->last_input_seq.load(std::memory_order_relaxed);
      while (header.seq > previous &&
             !conn->last_input_seq.compare_exchange_weak(
                 previous, header.seq, std::memory_order_relaxed)) {
      }
      if ((header.flags & BK_FLAG_MEASURE) != 0) {
        atomic_update_max(&conn->input_last_sent_measured, header.seq);
      }
    }
    metrics_on_slot(&header, submitted);
  }

  void mark_unsent_until(ClientConn *conn, uint64_t cutoff_ns) {
    bk_slot slot;
    while (bk_plan_next(conn->plan, cutoff_ns, &slot)) {
      bk_header header;
      make_header_from_slot(&slot, conn->origin_id, 0, &header);
      metrics_on_slot(&header, false);
    }
  }

  void record_recv(uint32_t local_index, const uint8_t *data, size_t len,
                   bool reliable_path) {
    bk_header header;
    if (bk_payload_read(data, len, &header) != 0) {
      invalid_payload_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (local_index >= conns_.size()) {
      invalid_payload_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (reliable_path !=
        ((header.flags & BK_FLAG_MUST_DELIVER) != 0)) {
      invalid_payload_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    ClientConn *conn = conns_[local_index];
    if (cfg_.scenario.kind == BK_SCENARIO_AUTHORITATIVE_STATE) {
      const bool must_deliver =
          (header.flags & BK_FLAG_MUST_DELIVER) != 0;
      uint64_t applied = 0;
      if (!bk_scenario_state_payload_valid(&cfg_.scenario, &header, len) ||
          bk_authoritative_state_read_applied_input_seq(data, len, &applied) !=
              0 ||
          applied > conn->last_input_seq.load(std::memory_order_relaxed) ||
          bk_authoritative_state_validate_target_pad(data, len,
                                                     conn->origin_id) != 0) {
        invalid_payload_.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      uint64_t previous =
          conn->last_applied_input_seq.load(std::memory_order_relaxed);
      while (applied > previous &&
             !conn->last_applied_input_seq.compare_exchange_weak(
                 previous, applied, std::memory_order_relaxed)) {
      }
      if (!must_deliver && (header.flags & BK_FLAG_MEASURE) != 0) {
        atomic_update_max(&conn->state_header_seq_recv_measured, header.seq);
        atomic_update_max(&conn->state_applied_input_seq_recv_measured,
                          applied);
      }
    } else {
      if ((cfg_.scenario.present &&
           !bk_scenario_client_payload_valid(&cfg_.scenario, &header, len)) ||
          bk_payload_validate_body(data, len, &header) != 0) {
        invalid_payload_.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
    const uint64_t now = bk_now_ns();
    std::lock_guard<std::mutex> lock(metrics_mu_);
    bk_metrics_on_recv(metrics_, local_index, &header, now);
  }

  int format_stats_json(char *buf, size_t cap) const {
    bk_authoritative_progress progress = {};
    if (cfg_.scenario.kind == BK_SCENARIO_AUTHORITATIVE_STATE) {
      progress.local_conns = static_cast<uint32_t>(cfg_.conns);
      progress.roster_conns = cfg_.scenario.total_conns;
      for (size_t i = 0; i < conns_.size(); ++i) {
        const ClientConn *conn = conns_[i];
        const uint64_t input =
            conn->input_last_sent_measured.load(std::memory_order_relaxed);
        const uint64_t state_header =
            conn->state_header_seq_recv_measured.load(std::memory_order_relaxed);
        const uint64_t state_applied = conn->state_applied_input_seq_recv_measured
                                           .load(std::memory_order_relaxed);
        if (i == 0 || input < progress.input_last_sent_min) {
          progress.input_last_sent_min = input;
        }
        if (input > progress.input_last_sent_max) {
          progress.input_last_sent_max = input;
        }
        if (i == 0 || state_header < progress.state_header_seq_recv_min) {
          progress.state_header_seq_recv_min = state_header;
        }
        if (state_header > progress.state_header_seq_recv_max) {
          progress.state_header_seq_recv_max = state_header;
        }
        if (i == 0 ||
            state_applied < progress.state_applied_input_seq_recv_min) {
          progress.state_applied_input_seq_recv_min = state_applied;
        }
        if (state_applied > progress.state_applied_input_seq_recv_max) {
          progress.state_applied_input_seq_recv_max = state_applied;
        }
      }
    }
    char progress_json[768];
    if (bk_authoritative_progress_format("client", &progress, progress_json,
                                         sizeof(progress_json)) != 0) {
      return -1;
    }
    const int n = std::snprintf(
        buf, cap, "{\"invalid_payload\":%" PRIu64 ",%s}",
        invalid_payload_.load(std::memory_order_relaxed), progress_json);
    return n > 0 && static_cast<size_t>(n) < cap ? 0 : -1;
  }

  static void atomic_update_max(std::atomic<uint64_t> *target,
                                uint64_t value) {
    uint64_t previous = target->load(std::memory_order_relaxed);
    while (value > previous &&
           !target->compare_exchange_weak(previous, value,
                                          std::memory_order_relaxed)) {
    }
  }

  void metrics_on_slot(const bk_header *header, bool submitted) {
    std::lock_guard<std::mutex> lock(metrics_mu_);
    bk_metrics_on_slot(metrics_, header, submitted);
  }

  void metrics_tick(uint64_t now) {
    std::lock_guard<std::mutex> lock(metrics_mu_);
    bk_metrics_tick(metrics_, now);
  }

  int dump_metrics(const char *path) {
    std::lock_guard<std::mutex> lock(metrics_mu_);
    return bk_metrics_dump_json(metrics_, path);
  }

  static QUIC_STATUS QUIC_API connection_cb(HQUIC /*connection*/, void *ctx,
                                            QUIC_CONNECTION_EVENT *event) {
    return static_cast<ClientConn *>(ctx)->app->on_connection_event(
        static_cast<ClientConn *>(ctx), event);
  }

  static QUIC_STATUS QUIC_API stream_cb(HQUIC stream, void *ctx,
                                        QUIC_STREAM_EVENT *event) {
    return static_cast<ClientConn *>(ctx)->app->on_stream_event(
        static_cast<ClientConn *>(ctx), stream, event);
  }

  ClientConfig cfg_;
  HQUIC registration_ = nullptr;
  HQUIC configuration_ = nullptr;
  std::vector<ClientConn *> conns_;
  bk_metrics *metrics_ = nullptr;
  std::mutex metrics_mu_;
  std::atomic<int> connected_count_{0};
  std::atomic<int> stream_ready_count_{0};
  std::atomic<bool> failed_{false};
  std::atomic<uint64_t> invalid_payload_{0};
};

}  // namespace

int main(int argc, char **argv) {
  ClientConfig cfg;
  if (parse_args(argc, argv, &cfg) != 0) {
    usage(argv[0]);
    return EXIT_FAILURE;
  }

  auto *app = new ClientApp(cfg);
  return app->run() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
