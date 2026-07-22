// rudp-bench v2 GameNetworkingSockets client。
// フラグ面は servers/enet/client.c と同一。app ロジックは main スレッド 1 本
// (RunCallbacks が status callback を同期発火する)、パケット I/O・暗号・再送は
// GNS 内部 service スレッドが担う。受信は全 conn を 1 つの poll group で drain
// し、conn の対応付けは k_ESteamNetworkingConfig_ConnectionUserData に埋めた
// local_index(受信側 dedup キー)で行う。
#include "benchkit.h"
#include "../scenario_cli.h"
#include "gns_common.h"

#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <unistd.h>

namespace {

constexpr uint64_t kDevWarmupNs = 200000000ull;
constexpr uint64_t kDevDurationNs = 2000000000ull;
constexpr uint64_t kDevDrainNs = 500000000ull;
constexpr uint64_t kConnectTimeoutNs = 10000000000ull;
constexpr uint64_t kServiceMaxSleepNs = 1000000ull;  // 1ms(受信は polling)
constexpr int kMaxConns = 4095;
constexpr int kRecvBatch = 256;
constexpr int kMaxDrainBatches = 16;  // 1 service あたり最大 4096 msg

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

struct ClientConn {
  HSteamNetConnection conn = k_HSteamNetConnection_Invalid;
  uint32_t origin_id = 0;
  uint32_t local_index = 0;  // 自 proc 内 0 起点(重複判定の受信側キー)
  bk_plan *plan = nullptr;
  bool connected = false;
  uint64_t last_input_seq = 0;
  uint64_t last_applied_input_seq = 0;
  uint64_t input_last_sent_measured = 0;
  uint64_t state_header_seq_recv_measured = 0;
  uint64_t state_applied_input_seq_recv_measured = 0;
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
      rudp_bench_gns::print_describe();
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
      have_conns = parse_i32(argv[++i], &cfg->conns) == 0 && cfg->conns > 0 &&
                   cfg->conns <= kMaxConns;
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
    if (std::strcmp(argv[i], "--staleness-period-ns") == 0 && i + 1 < argc) {
      have_staleness = parse_u64(argv[++i], &cfg->staleness_period_ns) == 0 &&
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
    if (bk_scenario_cli_validate(
            &cfg->scenario, kMaxConns,
            rudp_bench_gns::kMaxPayloadMustDeliver) != 0 ||
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
    auto valid_limits = [](const bk_scenario_traffic &value) {
      return (value.rate_lt == 0.0 ||
              value.payload_lt <=
                  rudp_bench_gns::kMaxPayloadLossTolerant) &&
             (value.rate_md == 0.0 ||
              value.payload_md <= rudp_bench_gns::kMaxPayloadMustDeliver);
    };
    if (!valid_limits(*traffic) ||
        (cfg->scenario.kind == BK_SCENARIO_AUTHORITATIVE_STATE &&
         !valid_limits(cfg->scenario.state))) {
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
  // 有効な stream の payload が class 別の送信上限内であること
  if (cfg->rate_lt > 0.0 &&
      (cfg->payload_lt < BK_MIN_PAYLOAD ||
       cfg->payload_lt > rudp_bench_gns::kMaxPayloadLossTolerant)) {
    return -1;
  }
  if (cfg->rate_md > 0.0 &&
      (cfg->payload_md < BK_MIN_PAYLOAD ||
       cfg->payload_md > rudp_bench_gns::kMaxPayloadMustDeliver)) {
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

int build_streams(const ClientConfig *cfg, bk_stream *streams, int *n_streams) {
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
  const int n = std::snprintf(buf, cap, "/tmp/rudp-bench-gns-client-%ld.json",
                              static_cast<long>(::getpid()));
  if (n <= 0 || static_cast<size_t>(n) >= cap) {
    return nullptr;
  }
  return buf;
}

uint64_t next_plan_due(const std::vector<ClientConn> &conns) {
  uint64_t next = UINT64_MAX;
  for (const ClientConn &conn : conns) {
    const uint64_t due = bk_plan_peek_ns(conn.plan);
    if (due < next) {
      next = due;
    }
  }
  return next;
}

uint64_t sleep_ns_for_next(uint64_t now_ns, uint64_t next_ns,
                           uint64_t limit_ns) {
  uint64_t wake_ns = next_ns;
  const uint64_t slice_ns = rudp_bench_gns::add_ns(now_ns, kServiceMaxSleepNs);
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

class ClientApp;
ClientApp *g_app = nullptr;

void status_callback(SteamNetConnectionStatusChangedCallback_t *info);

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
        bk_control_hello(control, "client", "gns", cfg_.proc_index) != 0) {
      std::fprintf(stderr, "benchkit hello failed\n");
      bk_control_close(control);
      return -1;
    }

    if (!open_connections()) {
      if (control != nullptr) {
        bk_control_close(control);
      }
      return -1;
    }
    if (!wait_for_ready()) {
      std::fprintf(stderr, "timed out waiting for GNS connections\n");
      if (control != nullptr) {
        bk_control_close(control);
      }
      return -1;
    }
    if (!send_registrations()) {
      std::fprintf(stderr, "GNS registration send failed\n");
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
      schedule.start_at_ns = rudp_bench_gns::add_ns(now, kDevWarmupNs);
      schedule.stop_at_ns =
          rudp_bench_gns::add_ns(schedule.start_at_ns, kDevDurationNs);
      schedule.drain_until_ns =
          rudp_bench_gns::add_ns(schedule.stop_at_ns, kDevDrainNs);
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
    for (ClientConn &conn : conns_) {
      conn.plan = bk_plan_new(streams, n_streams, plan_start_ns,
                              schedule.start_at_ns, schedule.stop_at_ns);
      if (conn.plan == nullptr) {
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
    const char *metrics_path = metrics_path_or_default(
        default_metrics_path, sizeof(default_metrics_path));
    if (metrics_path == nullptr ||
        bk_metrics_dump_json(metrics_, metrics_path) != 0) {
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

    teardown();
    return rc;
  }

  // RunCallbacks から同期的に呼ばれる(= main スレッド)。
  void on_status_changed(SteamNetConnectionStatusChangedCallback_t *info) {
    const int64_t user_data = info->m_info.m_nUserData;
    switch (info->m_info.m_eState) {
      case k_ESteamNetworkingConnectionState_Connected:
        if (user_data >= 0 && user_data < static_cast<int64_t>(conns_.size())) {
          ClientConn &conn = conns_[static_cast<size_t>(user_data)];
          if (!conn.connected) {
            conn.connected = true;
            connected_count_++;
          }
          iface_->SetConnectionPollGroup(conn.conn, poll_group_);
        }
        break;

      case k_ESteamNetworkingConnectionState_ClosedByPeer:
      case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        std::fprintf(stderr, "gns connection lost: %s\n",
                     info->m_info.m_szEndDebug);
        failed_ = true;
        break;

      default:
        break;
    }
  }

 private:
  bool open_connections() {
    rudp_bench_gns::ensure_gns();
    iface_ = SteamNetworkingSockets();
    utils_ = SteamNetworkingUtils();

    poll_group_ = iface_->CreatePollGroup();
    if (poll_group_ == k_HSteamNetPollGroup_Invalid) {
      std::fprintf(stderr, "CreatePollGroup failed\n");
      return false;
    }

    char addr_str[256];
    const int n = std::snprintf(addr_str, sizeof(addr_str), "%s:%" PRIu16,
                                cfg_.host, cfg_.port);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(addr_str)) {
      return false;
    }
    SteamNetworkingIPAddr addr;
    addr.Clear();
    if (!addr.ParseString(addr_str)) {
      std::fprintf(stderr, "invalid address %s (IP literal required)\n",
                   addr_str);
      return false;
    }

    conns_.resize(static_cast<size_t>(cfg_.conns));
    for (int i = 0; i < cfg_.conns; ++i) {
      ClientConn &conn = conns_[static_cast<size_t>(i)];
      conn.origin_id = cfg_.origin_base + static_cast<uint32_t>(i);
      conn.local_index = static_cast<uint32_t>(i);

      SteamNetworkingConfigValue_t opts[2];
      opts[0].SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
                     reinterpret_cast<void *>(status_callback));
      // 受信 message の m_nConnUserData → local_index の対応付けに使う。
      opts[1].SetInt64(k_ESteamNetworkingConfig_ConnectionUserData,
                       static_cast<int64>(i));
      conn.conn = iface_->ConnectByIPAddress(addr, 2, opts);
      if (conn.conn == k_HSteamNetConnection_Invalid) {
        std::fprintf(stderr, "ConnectByIPAddress failed\n");
        return false;
      }
    }
    return true;
  }

  bool wait_for_ready() {
    const uint64_t deadline =
        rudp_bench_gns::add_ns(bk_now_ns(), kConnectTimeoutNs);
    while (connected_count_ < cfg_.conns) {
      if (failed_ || bk_now_ns() >= deadline) {
        return false;
      }
      iface_->RunCallbacks();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  }

  bool send_registrations() {
    for (const ClientConn &conn : conns_) {
      uint8_t payload[BK_MIN_PAYLOAD] = {};
      const bk_header header = {
          0, 0, bk_now_ns(), 0, conn.origin_id, 0,
      };
      if (bk_payload_write(payload, sizeof(payload), &header) != 0 ||
          rudp_bench_gns::send_payload(iface_, conn.conn, payload,
                                       sizeof(payload), 0) != 0) {
        return false;
      }
    }
    return true;
  }

  bool expect_scenario_flows(const bk_schedule &schedule) {
    if (!cfg_.scenario.present) {
      return true;
    }
    if (cfg_.scenario.kind == BK_SCENARIO_AUTHORITATIVE_STATE) {
      if (cfg_.scenario.state.rate_lt == 0.0) {
        return true;
      }
      for (const ClientConn &conn : conns_) {
        if (bk_metrics_expect_latest(
                metrics_, conn.local_index, cfg_.scenario.total_conns,
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
    for (const ClientConn &conn : conns_) {
      const uint32_t first_origin =
          cfg_.scenario.kind == BK_SCENARIO_ROOM_RELAY ? 0u
                                                       : conn.origin_id;
      const uint32_t end_origin =
          cfg_.scenario.kind == BK_SCENARIO_ROOM_RELAY
              ? cfg_.scenario.total_conns
              : first_origin + 1u;
      for (uint32_t origin = first_origin; origin < end_origin; ++origin) {
        if (bk_metrics_expect_latest(metrics_, conn.local_index, origin,
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
      service();
      uint64_t now = bk_now_ns();
      if (failed_) {
        std::fprintf(stderr, "gns connection failed during run\n");
        rc = -1;
        break;
      }

      // 定常判定つき warmup(benchspec v2): rate 報告と確定窓(window)の受信。
      // window を受けたら全 conn の plan に計測窓を差し替える
      if (control != nullptr) {
        uint64_t raw_submitted = 0;
        uint64_t raw_rm = 0;
        uint64_t raw_ru = 0;
        bk_metrics_raw_counts(metrics_, nullptr, &raw_submitted, &raw_rm,
                              &raw_ru);
        const int sr = bk_steady_tick(&steady, control, raw_submitted,
                                      raw_rm + raw_ru, &schedule, now);
        if (sr < 0) {
          std::fprintf(stderr, "benchkit steady tick failed\n");
          rc = -1;
          break;
        }
        if (sr == 1) {
          for (ClientConn &conn : conns_) {
            bk_plan_set_window(conn.plan, schedule.start_at_ns,
                               schedule.stop_at_ns);
          }
        }
      }

      // staleness サンプルは計測窓内のみ(warmup / drain 混入は分布を汚染する)
      if (now >= schedule.start_at_ns && now < schedule.stop_at_ns) {
        bk_metrics_tick(metrics_, now);
      }

      if (now < schedule.stop_at_ns) {
        for (ClientConn &conn : conns_) {
          bk_slot slot;
          while (bk_plan_next(conn.plan, now, &slot)) {
            const size_t payload_size = (slot.flags & BK_FLAG_MUST_DELIVER)
                                            ? cfg_.payload_md
                                            : cfg_.payload_lt;
            send_slot(&conn, &slot, payload_size);
          }
        }
      } else if (!marked_unsent) {
        const uint64_t cutoff =
            schedule.stop_at_ns == 0 ? 0 : schedule.stop_at_ns - 1u;
        for (ClientConn &conn : conns_) {
          bk_slot slot;
          while (bk_plan_next(conn.plan, cutoff, &slot)) {
            const size_t payload_size = (slot.flags & BK_FLAG_MUST_DELIVER)
                                            ? cfg_.payload_md
                                            : cfg_.payload_lt;
            send_slot(&conn, &slot, payload_size);
          }
        }
        for (ClientConn &conn : conns_) {
          mark_unsent_until(&conn, cutoff);
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
      for (ClientConn &conn : conns_) {
        mark_unsent_until(&conn, cutoff);
      }
    }
    return rc;
  }

  // RunCallbacks + poll group の受信 drain。
  // drain は batch 数で bound する: broadcast fanout(受信 conns² スケール)では
  // 上限なしだとこのループから抜けられず、送信 pacing と bk_steady_tick が
  // 飢える(docs/ledger.md #13)。受信キューは GNS 内部スレッドが埋め続ける
  // ので、残りは次呼び出しで続きを引けばよい。
  void service() {
    iface_->RunCallbacks();
    SteamNetworkingMessage_t *msgs[kRecvBatch];
    for (int batch = 0; batch < kMaxDrainBatches; ++batch) {
      const int n =
          iface_->ReceiveMessagesOnPollGroup(poll_group_, msgs, kRecvBatch);
      if (n <= 0) {
        break;
      }
      const uint64_t now = bk_now_ns();
      for (int i = 0; i < n; ++i) {
        const int64_t user_data = msgs[i]->m_nConnUserData;
        bk_header header;
        bool accept = user_data >= 0 &&
            user_data < static_cast<int64_t>(conns_.size()) &&
            bk_payload_read(msgs[i]->m_pData,
                            static_cast<size_t>(msgs[i]->m_cbSize),
                            &header) == 0 &&
            ((msgs[i]->m_nFlags & k_nSteamNetworkingSend_Reliable) != 0) ==
                ((header.flags & BK_FLAG_MUST_DELIVER) != 0);
        if (accept) {
          ClientConn &conn = conns_[static_cast<size_t>(user_data)];
          const auto *payload =
              static_cast<const uint8_t *>(msgs[i]->m_pData);
          const size_t payload_size =
              static_cast<size_t>(msgs[i]->m_cbSize);
          if (cfg_.scenario.kind == BK_SCENARIO_AUTHORITATIVE_STATE) {
            const bool must_deliver =
                (header.flags & BK_FLAG_MUST_DELIVER) != 0;
            uint64_t applied = 0;
            accept = bk_scenario_state_payload_valid(
                         &cfg_.scenario, &header, payload_size) &&
                     bk_authoritative_state_read_applied_input_seq(
                         payload, payload_size, &applied) == 0 &&
                     applied <= conn.last_input_seq &&
                     bk_authoritative_state_validate_target_pad(
                         payload, payload_size, conn.origin_id) == 0;
            if (accept && applied > conn.last_applied_input_seq) {
              conn.last_applied_input_seq = applied;
            }
            if (accept && !must_deliver &&
                (header.flags & BK_FLAG_MEASURE) != 0) {
              if (header.seq > conn.state_header_seq_recv_measured) {
                conn.state_header_seq_recv_measured = header.seq;
              }
              if (applied > conn.state_applied_input_seq_recv_measured) {
                conn.state_applied_input_seq_recv_measured = applied;
              }
            }
          } else {
            if (cfg_.scenario.present) {
              accept = bk_scenario_client_payload_valid(
                  &cfg_.scenario, &header, payload_size);
            }
            if (accept) {
              accept = bk_payload_validate_body(payload, payload_size,
                                                &header) == 0;
            }
          }
          if (accept) {
            bk_metrics_on_recv(metrics_, conn.local_index, &header, now);
          }
        }
        if (!accept) {
          invalid_payload_++;
        }
        msgs[i]->Release();
      }
      if (n < kRecvBatch) {
        break;
      }
    }
  }

  // AllocateMessage のバッファへ header と deterministic body pattern を直接
  // 書き、SendMessages で所有権ごと渡す。SendMessageToConnection の enqueue
  // 時 malloc+memcpy を避ける(connections.cpp:2038,2044)。
  void send_slot(ClientConn *conn, const bk_slot *slot, size_t payload_size) {
    bk_header header;
    make_header_from_slot(slot, conn->origin_id, bk_now_ns(), &header);
    bool submitted = false;
    SteamNetworkingMessage_t *msg =
        utils_->AllocateMessage(static_cast<int>(payload_size));
    if (msg != nullptr) {
      if (bk_payload_write(msg->m_pData, payload_size, &header) == 0 &&
          bk_payload_fill_body(msg->m_pData, payload_size, &header) == 0) {
        msg->m_conn = conn->conn;
        msg->m_nFlags = rudp_bench_gns::send_flags_for(header.flags);
        int64 result = 0;
        iface_->SendMessages(1, &msg, &result);
        submitted = result > 0;
      } else {
        msg->Release();
      }
    }
    bk_metrics_on_slot(metrics_, &header, submitted);
    if (submitted && BK_FLAGS_DIRECTION(header.flags) ==
                         BK_DIRECTION_CLIENT_TO_SERVER &&
        (header.flags & BK_FLAG_MUST_DELIVER) == 0 &&
        header.seq > conn->last_input_seq) {
      conn->last_input_seq = header.seq;
    }
    if (submitted && BK_FLAGS_DIRECTION(header.flags) ==
                         BK_DIRECTION_CLIENT_TO_SERVER &&
        (header.flags & (BK_FLAG_MUST_DELIVER | BK_FLAG_MEASURE)) ==
            BK_FLAG_MEASURE &&
        header.seq > conn->input_last_sent_measured) {
      conn->input_last_sent_measured = header.seq;
    }
  }

  int format_stats_json(char *buf, size_t cap) const {
    bk_authoritative_progress progress = {};
    if (cfg_.scenario.kind == BK_SCENARIO_AUTHORITATIVE_STATE) {
      progress.local_conns = static_cast<uint32_t>(cfg_.conns);
      progress.roster_conns = cfg_.scenario.total_conns;
      for (size_t i = 0; i < conns_.size(); ++i) {
        const ClientConn &conn = conns_[i];
        if (i == 0 || conn.input_last_sent_measured <
                          progress.input_last_sent_min) {
          progress.input_last_sent_min = conn.input_last_sent_measured;
        }
        if (conn.input_last_sent_measured > progress.input_last_sent_max) {
          progress.input_last_sent_max = conn.input_last_sent_measured;
        }
        if (i == 0 || conn.state_header_seq_recv_measured <
                          progress.state_header_seq_recv_min) {
          progress.state_header_seq_recv_min =
              conn.state_header_seq_recv_measured;
        }
        if (conn.state_header_seq_recv_measured >
            progress.state_header_seq_recv_max) {
          progress.state_header_seq_recv_max =
              conn.state_header_seq_recv_measured;
        }
        if (i == 0 || conn.state_applied_input_seq_recv_measured <
                          progress.state_applied_input_seq_recv_min) {
          progress.state_applied_input_seq_recv_min =
              conn.state_applied_input_seq_recv_measured;
        }
        if (conn.state_applied_input_seq_recv_measured >
            progress.state_applied_input_seq_recv_max) {
          progress.state_applied_input_seq_recv_max =
              conn.state_applied_input_seq_recv_measured;
        }
      }
    }
    char progress_json[768];
    if (bk_authoritative_progress_format("client", &progress, progress_json,
                                         sizeof(progress_json)) != 0) {
      return -1;
    }
    const int n = std::snprintf(buf, cap,
                                "{\"invalid_payload\":%" PRIu64 ",%s}",
                                invalid_payload_, progress_json);
    return n > 0 && static_cast<size_t>(n) < cap ? 0 : -1;
  }

  void mark_unsent_until(ClientConn *conn, uint64_t cutoff_ns) {
    bk_slot slot;
    while (bk_plan_next(conn->plan, cutoff_ns, &slot)) {
      bk_header header;
      make_header_from_slot(&slot, conn->origin_id, 0, &header);
      bk_metrics_on_slot(metrics_, &header, false);
    }
  }

  void teardown() {
    for (ClientConn &conn : conns_) {
      if (conn.plan != nullptr) {
        bk_plan_free(conn.plan);
        conn.plan = nullptr;
      }
      if (conn.conn != k_HSteamNetConnection_Invalid) {
        iface_->CloseConnection(conn.conn, 0, nullptr, false);
        conn.conn = k_HSteamNetConnection_Invalid;
      }
    }
    if (poll_group_ != k_HSteamNetPollGroup_Invalid) {
      iface_->DestroyPollGroup(poll_group_);
      poll_group_ = k_HSteamNetPollGroup_Invalid;
    }
  }

  ClientConfig cfg_;
  ISteamNetworkingSockets *iface_ = nullptr;
  ISteamNetworkingUtils *utils_ = nullptr;
  HSteamNetPollGroup poll_group_ = k_HSteamNetPollGroup_Invalid;
  std::vector<ClientConn> conns_;
  bk_metrics *metrics_ = nullptr;
  int connected_count_ = 0;
  bool failed_ = false;
  uint64_t invalid_payload_ = 0;
};

void status_callback(SteamNetConnectionStatusChangedCallback_t *info) {
  if (g_app != nullptr) {
    g_app->on_status_changed(info);
  }
}

}  // namespace

int main(int argc, char **argv) {
  ClientConfig cfg;
  if (parse_args(argc, argv, &cfg) != 0) {
    usage(argv[0]);
    return EXIT_FAILURE;
  }

  auto *app = new ClientApp(cfg);
  g_app = app;
  return app->run() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
