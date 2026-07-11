// rudp-bench v2 GameNetworkingSockets server。
// benchspec/README.md の server 意味論に従う: 設定を持たず、受信 payload の
// flags だけで echo / broadcast を決める。app ロジックは main スレッド 1 本
// (RunCallbacks が status callback を同期発火する)、パケット I/O・暗号・再送は
// GNS 内部 service スレッドが担う。
#include "benchkit.h"
#include "../scenario_cli.h"
#include "gns_common.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

// broadcast fanout 用の共有 payload バッファ。GNS に同報 API は無く、
// SendMessageToConnection は enqueue のたびに malloc+memcpy する
// (connections.cpp:2038,2044)。AllocateMessage(0) で struct だけ確保して
// m_pData を共有バッファへ向け、m_pfnFreeData の refcount で寿命管理するのが
// upstream が保証する最小コスト経路(isteamnetworkingsockets.h:276-293)。
// 解放 callback は任意スレッドから呼ばれうるため refcount は atomic。
struct SharedPayload {
  std::atomic<int> refs;
  alignas(std::max_align_t) uint8_t data[1];  // 実体は malloc で len 分連続確保
};

SharedPayload *shared_payload_new(const uint8_t *src, size_t len, int refs) {
  auto *sp = static_cast<SharedPayload *>(
      std::malloc(offsetof(SharedPayload, data) + len));
  if (sp == nullptr) {
    return nullptr;
  }
  new (&sp->refs) std::atomic<int>(refs);
  std::memcpy(sp->data, src, len);
  return sp;
}

void shared_payload_free_cb(SteamNetworkingMessage_t *msg) {
  auto *sp = reinterpret_cast<SharedPayload *>(
      static_cast<uint8_t *>(msg->m_pData) - offsetof(SharedPayload, data));
  if (sp->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    std::free(sp);
  }
}

constexpr int kRecvBatch = 256;
constexpr int kMaxDrainBatches = 16;  // 1 service あたり最大 4096 msg
constexpr uint64_t kServiceSliceNs = 1000000ull;  // 1ms(アイドル時のみ)
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

struct ServerStats {
  uint64_t recv[CLASS_COUNT][DIST_COUNT] = {};
  uint64_t recv_measured[CLASS_COUNT][DIST_COUNT] = {};
  uint64_t submit[CLASS_COUNT][DIST_COUNT] = {};
  uint64_t submit_measured[CLASS_COUNT][DIST_COUNT] = {};
  uint64_t send_failed[CLASS_COUNT][DIST_COUNT] = {};
  uint64_t invalid_payload = 0;
  uint64_t server_state_ticks = 0;
};

struct ServerConfig {
  uint16_t port = 0;
  uint64_t staleness_period_ns = 10000000ull;
  bk_scenario_cli scenario = {};
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
      rudp_bench_gns::print_describe();
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
  if (!have_port ||
      bk_scenario_cli_validate(
          &cfg->scenario, kMaxConns,
          rudp_bench_gns::kMaxPayloadMustDeliver) != 0) {
    return -1;
  }
  const auto valid_limits = [](const bk_scenario_traffic &value) {
    return (value.rate_lt == 0.0 ||
            value.payload_lt <= rudp_bench_gns::kMaxPayloadLossTolerant) &&
           (value.rate_md == 0.0 ||
            value.payload_md <= rudp_bench_gns::kMaxPayloadMustDeliver);
  };
  if (cfg->scenario.present) {
    const bk_scenario_traffic *traffic = nullptr;
    if (cfg->scenario.kind == BK_SCENARIO_AUTHORITATIVE_STATE) {
      if (!valid_limits(cfg->scenario.input) ||
          !valid_limits(cfg->scenario.state)) {
        return -1;
      }
    } else {
      traffic = cfg->scenario.kind == BK_SCENARIO_ROOM_RELAY
                    ? &cfg->scenario.publish
                    : &cfg->scenario.input;
      if (!valid_limits(*traffic)) {
        return -1;
      }
    }
  }
  return 0;
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

uint64_t mark_state_unsent(const ServerConfig &cfg, bk_metrics *metrics,
                           bk_plan *plan, uint64_t cutoff_ns) {
  uint64_t measured_lt_ticks = 0;
  bk_slot slot;
  while (bk_plan_next(plan, cutoff_ns, &slot)) {
    if ((slot.flags & (BK_FLAG_MEASURE | BK_FLAG_MUST_DELIVER)) ==
        BK_FLAG_MEASURE) {
      measured_lt_ticks++;
    }
    for (uint32_t target = 0; target < cfg.scenario.total_conns; ++target) {
      (void)target;
      bk_header header = {};
      header.seq = slot.seq;
      header.sched_ts_ns = slot.sched_ts_ns;
      header.flags = static_cast<uint8_t>(slot.flags & ~BK_FLAG_BROADCAST);
      header.origin_id = cfg.scenario.total_conns;
      header.traffic_id = slot.traffic_id;
      bk_metrics_on_slot(metrics, &header, false);
    }
  }
  return measured_lt_ticks;
}

class ServerApp;
ServerApp *g_app = nullptr;

void status_callback(SteamNetConnectionStatusChangedCallback_t *info);

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
      roster_.assign(cfg_.scenario.total_conns,
                     k_HSteamNetConnection_Invalid);
      applied_input_seq_.assign(cfg_.scenario.total_conns, 0);
    }
    rudp_bench_gns::ensure_gns();
    iface_ = SteamNetworkingSockets();
    utils_ = SteamNetworkingUtils();

    poll_group_ = iface_->CreatePollGroup();
    if (poll_group_ == k_HSteamNetPollGroup_Invalid) {
      std::fprintf(stderr, "CreatePollGroup failed\n");
      return false;
    }

    SteamNetworkingIPAddr addr;
    addr.Clear();
    addr.m_port = port;

    SteamNetworkingConfigValue_t opt;
    opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged,
               reinterpret_cast<void *>(status_callback));
    listen_sock_ = iface_->CreateListenSocketIP(addr, 1, &opt);
    if (listen_sock_ == k_HSteamListenSocket_Invalid) {
      std::fprintf(stderr, "CreateListenSocketIP failed on port %" PRIu16 "\n",
                    port);
      return false;
    }
    return true;
  }

  // RunCallbacks(status callback を同期発火)+ poll group の受信 drain。
  // 戻り値: 処理した message 数 / -1=致命的エラー。
  // drain は batch 数で bound する(制御チャネル poll を飢えさせない。
  // docs/ledger.md #13 と同族)。残りは次呼び出しで続きを引く。
  int service() {
    iface_->RunCallbacks();
    SteamNetworkingMessage_t *msgs[kRecvBatch];
    int total = 0;
    for (int batch = 0; batch < kMaxDrainBatches; ++batch) {
      const int n =
          iface_->ReceiveMessagesOnPollGroup(poll_group_, msgs, kRecvBatch);
      if (n < 0) {
        return -1;
      }
      if (n == 0) {
        break;
      }
      for (int i = 0; i < n; ++i) {
        handle_payload(msgs[i]->m_conn,
                       static_cast<const uint8_t *>(msgs[i]->m_pData),
                       static_cast<size_t>(msgs[i]->m_cbSize),
                       msgs[i]->m_nFlags, msgs[i]->m_nConnUserData);
        msgs[i]->Release();
      }
      total += n;
      if (n < kRecvBatch) {
        break;
      }
    }
    return total;
  }

  // RunCallbacks から同期的に呼ばれる(= main スレッド)。
  void on_status_changed(SteamNetConnectionStatusChangedCallback_t *info) {
    const HSteamNetConnection conn = info->m_hConn;
    switch (info->m_info.m_eState) {
      case k_ESteamNetworkingConnectionState_Connecting:
        // listen socket 経由の新着接続のみ accept する。
        if (info->m_info.m_hListenSocket != k_HSteamListenSocket_Invalid &&
            iface_->AcceptConnection(conn) != k_EResultOK) {
          iface_->CloseConnection(conn, 0, nullptr, false);
        }
        break;

      case k_ESteamNetworkingConnectionState_Connected:
        iface_->SetConnectionPollGroup(conn, poll_group_);
        iface_->SetConnectionUserData(conn, -1);
        conns_.push_back(conn);
        active_conns_.insert(conn);
        break;

      case k_ESteamNetworkingConnectionState_ClosedByPeer:
      case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        {
          const int64 registered_origin = iface_->GetConnectionUserData(conn);
          if (registered_origin >= 0 &&
              registered_origin < static_cast<int64>(roster_.size()) &&
              roster_[static_cast<size_t>(registered_origin)] == conn) {
            roster_[static_cast<size_t>(registered_origin)] =
                k_HSteamNetConnection_Invalid;
            if (!roster_frozen_) {
              registered_count_--;
            }
          }
        }
        active_conns_.erase(conn);
        iface_->CloseConnection(conn, 0, nullptr, false);
        for (size_t i = 0; i < conns_.size(); ++i) {
          if (conns_[i] == conn) {
            conns_[i] = conns_.back();
            conns_.pop_back();
            break;
          }
        }
        break;

      default:
        break;
    }
  }

  void shutdown() {
    if (listen_sock_ != k_HSteamListenSocket_Invalid) {
      iface_->CloseListenSocket(listen_sock_);
      listen_sock_ = k_HSteamListenSocket_Invalid;
    }
    for (const HSteamNetConnection conn : conns_) {
      iface_->CloseConnection(conn, 0, nullptr, false);
    }
    conns_.clear();
    if (poll_group_ != k_HSteamNetPollGroup_Invalid) {
      iface_->DestroyPollGroup(poll_group_);
      poll_group_ = k_HSteamNetPollGroup_Invalid;
    }
    bk_metrics_free(metrics_);
    metrics_ = nullptr;
  }

  bool roster_complete() const {
    return cfg_.scenario.kind == BK_SCENARIO_AUTHORITATIVE_STATE &&
           registered_count_ == cfg_.scenario.total_conns;
  }

  void freeze_roster() { roster_frozen_ = true; }

  bk_metrics *metrics() const { return metrics_; }

  int expect_client_inputs(const bk_schedule &schedule) {
    if (cfg_.scenario.input.rate_lt == 0.0) {
      return 0;
    }
    for (uint32_t origin = 0; origin < cfg_.scenario.total_conns; ++origin) {
      if (bk_metrics_expect_latest(
              metrics_, origin, origin, cfg_.scenario.input.traffic_id,
              BK_DIRECTION_CLIENT_TO_SERVER, schedule.start_at_ns) != 0) {
        return -1;
      }
    }
    return 0;
  }

  void send_state_slot(const bk_slot &slot) {
    if ((slot.flags & (BK_FLAG_MEASURE | BK_FLAG_MUST_DELIVER)) ==
        BK_FLAG_MEASURE) {
      stats_.server_state_ticks++;
    }
    const size_t payload_size =
        (slot.flags & BK_FLAG_MUST_DELIVER) != 0
            ? cfg_.scenario.state.payload_md
            : cfg_.scenario.state.payload_lt;
    for (uint32_t target = 0; target < cfg_.scenario.total_conns; ++target) {
      bk_header header = {};
      header.seq = slot.seq;
      header.sched_ts_ns = slot.sched_ts_ns;
      header.flags = static_cast<uint8_t>(slot.flags & ~BK_FLAG_BROADCAST);
      header.origin_id = cfg_.scenario.total_conns;
      header.traffic_id = slot.traffic_id;
      bool submitted = false;
      SteamNetworkingMessage_t *msg =
          utils_->AllocateMessage(static_cast<int>(payload_size));
      if (msg != nullptr &&
          bk_authoritative_state_write_applied_input_seq(
              msg->m_pData, payload_size, applied_input_seq_[target]) == 0 &&
          bk_authoritative_state_fill_target_pad(msg->m_pData, payload_size,
                                                  target) == 0 &&
          roster_[target] != k_HSteamNetConnection_Invalid) {
        header.send_ts_ns = bk_now_ns();
        if (bk_payload_write(msg->m_pData, payload_size, &header) != 0) {
          msg->Release();
          msg = nullptr;
        }
      }
      if (msg != nullptr &&
          roster_[target] != k_HSteamNetConnection_Invalid &&
          header.send_ts_ns != 0) {
        msg->m_conn = roster_[target];
        msg->m_nFlags = rudp_bench_gns::send_flags_for(header.flags);
        int64 result = 0;
        iface_->SendMessages(1, &msg, &result);
        submitted = result > 0;
      } else if (msg != nullptr) {
        msg->Release();
      }
      bk_metrics_on_slot(metrics_, &header, submitted);
    }
  }

  int format_stats_json(char *buf, size_t cap) const {
    const ServerStats &s = stats_;
    bk_authoritative_progress progress = {};
    if (cfg_.scenario.kind == BK_SCENARIO_AUTHORITATIVE_STATE) {
      progress.roster_conns = registered_count_;
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
    const int suffix = std::snprintf(buf + prefix, cap - prefix, "%s}",
                                     progress_json);
    return suffix > 0 && static_cast<size_t>(suffix) < cap - prefix ? 0 : -1;
  }

  void add_server_state_ticks(uint64_t ticks) {
    stats_.server_state_ticks += ticks;
  }

 private:
  void handle_payload(HSteamNetConnection origin, const uint8_t *data,
                      size_t len, int transport_flags,
                      int64 captured_origin) {
    // A message can remain queued after its disconnect callback has closed the
    // handle. Never call connection APIs for a handle no longer in our set.
    if (active_conns_.find(origin) == active_conns_.end()) {
      return;
    }
    bk_header header;
    if (bk_payload_read(data, len, &header) != 0) {
      stats_.invalid_payload++;
      return;
    }
    const bool transport_reliable =
        (transport_flags & k_nSteamNetworkingSend_Reliable) != 0;
    const bool header_reliable =
        (header.flags & BK_FLAG_MUST_DELIVER) != 0;
    if (transport_reliable != header_reliable) {
      stats_.invalid_payload++;
      return;
    }

    const bool registration = bk_payload_is_registration(&header, len) != 0;
    if (registration && cfg_.scenario.present &&
        header.origin_id >= cfg_.scenario.total_conns) {
      stats_.invalid_payload++;
      return;
    }
    if (!registration &&
        ((cfg_.scenario.present &&
          !bk_scenario_client_payload_valid(&cfg_.scenario, &header, len)) ||
         bk_payload_validate_body(data, len, &header) != 0)) {
      stats_.invalid_payload++;
      return;
    }
    if (registration &&
        cfg_.scenario.kind != BK_SCENARIO_AUTHORITATIVE_STATE) {
      return;
    }

    int64 registered_origin = captured_origin;
    if (cfg_.scenario.kind == BK_SCENARIO_AUTHORITATIVE_STATE) {
      if (!registration &&
          !bk_scenario_client_payload_valid(&cfg_.scenario, &header, len)) {
        stats_.invalid_payload++;
        return;
      }
      if (registered_origin < 0 && roster_[header.origin_id] == origin) {
        // Messages already queued when registration was processed retain the
        // previous (-1) connection-user-data snapshot.
        registered_origin = static_cast<int64>(header.origin_id);
      }
      if (registered_origin < 0) {
        if (roster_frozen_ ||
            roster_[header.origin_id] != k_HSteamNetConnection_Invalid ||
            !iface_->SetConnectionUserData(
                origin, static_cast<int64>(header.origin_id))) {
          stats_.invalid_payload++;
          return;
        }
        registered_origin = static_cast<int64>(header.origin_id);
        roster_[header.origin_id] = origin;
        applied_input_seq_[header.origin_id] = 0;
        registered_count_++;
      } else if (registered_origin != static_cast<int64>(header.origin_id) ||
                 registered_origin >= static_cast<int64>(roster_.size()) ||
                 roster_[static_cast<size_t>(registered_origin)] != origin) {
        stats_.invalid_payload++;
        return;
      }
      if (registration) {
        return;
      }
    }

    const ClassIndex cls = class_from_flags(header.flags);
    const DistIndex dist = dist_from_flags(header.flags);
    const bool measured = (header.flags & BK_FLAG_MEASURE) != 0;
    stats_.recv[cls][dist]++;
    if (measured) {
      stats_.recv_measured[cls][dist]++;
    }

    if (cfg_.scenario.kind == BK_SCENARIO_AUTHORITATIVE_STATE) {
      const size_t target = static_cast<size_t>(registered_origin);
      if ((header.flags & BK_FLAG_MUST_DELIVER) == 0 &&
          header.seq > applied_input_seq_[target]) {
        applied_input_seq_[target] = header.seq;
      }
      bk_metrics_on_recv(metrics_, static_cast<uint32_t>(target), &header,
                         bk_now_ns());
      return;
    }

    if (dist == DIST_ECHO) {
      const bool ok =
          rudp_bench_gns::send_payload(iface_, origin, data, len,
                                       header.flags) == 0;
      count_submit(cls, dist, measured, ok);
      return;
    }

    // broadcast: 現在の全接続(origin 含む)へ無変更で fanout。
    // payload は SharedPayload で 1 回だけ copy し、conn ごとには
    // AllocateMessage(0) の struct のみ確保して SendMessages で一括投入する
    // (同一 conn 連続分のロック取り直しも避けられる、
    // csteamnetworkingsockets.cpp:1338-1353)。
    const int n_conns = static_cast<int>(conns_.size());
    if (n_conns == 0) {
      return;
    }
    SharedPayload *sp = shared_payload_new(data, len, n_conns);
    if (sp == nullptr) {
      for (int i = 0; i < n_conns; ++i) {
        count_submit(cls, dist, measured, false);
      }
      return;
    }
    const int send_flags = rudp_bench_gns::send_flags_for(header.flags);
    bcast_msgs_.resize(static_cast<size_t>(n_conns));
    bcast_results_.resize(static_cast<size_t>(n_conns));
    for (int i = 0; i < n_conns; ++i) {
      SteamNetworkingMessage_t *msg = utils_->AllocateMessage(0);
      msg->m_pData = sp->data;
      msg->m_cbSize = static_cast<uint32>(len);
      msg->m_pfnFreeData = shared_payload_free_cb;
      msg->m_conn = conns_[static_cast<size_t>(i)];
      msg->m_nFlags = send_flags;
      bcast_msgs_[static_cast<size_t>(i)] = msg;
    }
    iface_->SendMessages(n_conns, bcast_msgs_.data(), bcast_results_.data());
    for (int i = 0; i < n_conns; ++i) {
      count_submit(cls, dist, measured, bcast_results_[static_cast<size_t>(i)] > 0);
    }
  }

  void count_submit(ClassIndex cls, DistIndex dist, bool measured, bool ok) {
    if (ok) {
      stats_.submit[cls][dist]++;
      if (measured) {
        stats_.submit_measured[cls][dist]++;
      }
    } else {
      stats_.send_failed[cls][dist]++;
    }
  }

  ISteamNetworkingSockets *iface_ = nullptr;
  ISteamNetworkingUtils *utils_ = nullptr;
  HSteamListenSocket listen_sock_ = k_HSteamListenSocket_Invalid;
  HSteamNetPollGroup poll_group_ = k_HSteamNetPollGroup_Invalid;
  std::vector<HSteamNetConnection> conns_;
  std::unordered_set<HSteamNetConnection> active_conns_;
  std::vector<HSteamNetConnection> roster_;
  std::vector<uint64_t> applied_input_seq_;
  uint32_t registered_count_ = 0;
  bool roster_frozen_ = false;
  std::vector<SteamNetworkingMessage_t *> bcast_msgs_;   // fanout scratch
  std::vector<int64> bcast_results_;
  ServerStats stats_;
  ServerConfig cfg_;
  bk_metrics *metrics_ = nullptr;
};

void status_callback(SteamNetConnectionStatusChangedCallback_t *info) {
  if (g_app != nullptr) {
    g_app->on_status_changed(info);
  }
}

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
  g_app = app;
  if (!app->start(cfg.port)) {
    return EXIT_FAILURE;
  }

  bk_control *control = bk_control_connect(nullptr);
  bk_schedule schedule = {0, 0, 0};
  if (control != nullptr) {
    if (bk_control_hello(control, "server", "gns", 0) != 0 ||
        bk_control_ready(control, 0) != 0) {
      std::fprintf(stderr, "benchkit control handshake failed\n");
      bk_control_close(control);
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
        std::fprintf(stderr, "benchkit schedule wait failed\n");
        bk_control_close(control);
        return EXIT_FAILURE;
      }
      if (app->service() < 0) {
        std::fprintf(stderr, "gns service failed\n");
        bk_control_close(control);
        return EXIT_FAILURE;
      }
      std::this_thread::sleep_for(std::chrono::nanoseconds(kServiceSliceNs));
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
    const int serviced = app->service();
    if (serviced < 0) {
      std::fprintf(stderr, "gns service failed\n");
      run_rc = -1;
      break;
    }
    uint64_t now = bk_now_ns();
    if (authoritative && !schedule_valid && app->roster_complete()) {
      schedule.start_at_ns =
          rudp_bench_gns::add_ns(now, kDevWarmupNs);
      schedule.stop_at_ns =
          rudp_bench_gns::add_ns(schedule.start_at_ns, kDevDurationNs);
      schedule.drain_until_ns =
          rudp_bench_gns::add_ns(schedule.stop_at_ns, kDevDrainNs);
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
        app->roster_complete()) {
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
      app->freeze_roster();
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
      bk_metrics_tick(app->metrics(), now);
    }
    if (state_plan != nullptr && now < schedule.stop_at_ns) {
      bk_slot slot;
      while (bk_plan_next(state_plan, now, &slot)) {
        app->send_state_slot(slot);
      }
    } else if (state_plan != nullptr && !state_marked_unsent) {
      const uint64_t cutoff =
          schedule.stop_at_ns == 0 ? 0 : schedule.stop_at_ns - 1u;
      app->add_server_state_ticks(
          mark_state_unsent(cfg, app->metrics(), state_plan, cutoff));
      state_marked_unsent = true;
    }
    if (control != nullptr && now >= schedule.drain_until_ns) {
      break;
    }

    uint64_t sleep_ns = kServiceSliceNs;
    if (state_plan != nullptr && now < schedule.stop_at_ns) {
      const uint64_t due = bk_plan_peek_ns(state_plan);
      if (due <= now) {
        sleep_ns = 0;
      } else if (due - now < sleep_ns) {
        sleep_ns = due - now;
      }
    }
    if (serviced < kRecvBatch && sleep_ns != 0) {
      std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
    }
  }

  if (state_plan != nullptr && !state_marked_unsent) {
    const uint64_t cutoff =
        schedule.stop_at_ns == 0 ? 0 : schedule.stop_at_ns - 1u;
    app->add_server_state_ticks(
        mark_state_unsent(cfg, app->metrics(), state_plan, cutoff));
  }

  char stats_json[4096];
  if (app->format_stats_json(stats_json, sizeof(stats_json)) != 0) {
    std::fprintf(stderr, "server stats JSON overflow\n");
    if (control != nullptr) {
      bk_control_close(control);
    }
    return EXIT_FAILURE;
  }

  if (write_metrics_out(app->metrics()) != 0) {
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
  app->shutdown();
  return run_rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
