// rudp-bench v2 GameNetworkingSockets server。
// benchspec/README.md の server 意味論に従う: 設定を持たず、受信 payload の
// flags だけで echo / broadcast を決める。app ロジックは main スレッド 1 本
// (RunCallbacks が status callback を同期発火する)、パケット I/O・暗号・再送は
// GNS 内部 service スレッドが担う。
#include "benchkit.h"
#include "gns_common.h"

#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

constexpr int kRecvBatch = 256;
constexpr uint64_t kServiceSliceNs = 1000000ull;  // 1ms

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
      rudp_bench_gns::print_describe();
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

class ServerApp;
ServerApp *g_app = nullptr;

void status_callback(SteamNetConnectionStatusChangedCallback_t *info);

class ServerApp {
 public:
  bool start(uint16_t port) {
    rudp_bench_gns::ensure_gns();
    iface_ = SteamNetworkingSockets();

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
  // 戻り値 0=ok / -1=致命的エラー。
  int service() {
    iface_->RunCallbacks();
    SteamNetworkingMessage_t *msgs[kRecvBatch];
    for (;;) {
      const int n =
          iface_->ReceiveMessagesOnPollGroup(poll_group_, msgs, kRecvBatch);
      if (n < 0) {
        return -1;
      }
      if (n == 0) {
        return 0;
      }
      for (int i = 0; i < n; ++i) {
        handle_payload(msgs[i]->m_conn,
                       static_cast<const uint8_t *>(msgs[i]->m_pData),
                       static_cast<size_t>(msgs[i]->m_cbSize));
        msgs[i]->Release();
      }
      if (n < kRecvBatch) {
        return 0;
      }
    }
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
        conns_.push_back(conn);
        break;

      case k_ESteamNetworkingConnectionState_ClosedByPeer:
      case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
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
  }

  int format_stats_json(char *buf, size_t cap) const {
    const ServerStats &s = stats_;
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
  void handle_payload(HSteamNetConnection origin, const uint8_t *data,
                      size_t len) {
    bk_header header;
    if (bk_payload_read(data, len, &header) != 0) {
      stats_.invalid_payload++;
      return;
    }

    const ClassIndex cls = class_from_flags(header.flags);
    const DistIndex dist = dist_from_flags(header.flags);
    const bool measured = (header.flags & BK_FLAG_MEASURE) != 0;
    stats_.recv[cls][dist]++;
    if (measured) {
      stats_.recv_measured[cls][dist]++;
    }

    if (dist == DIST_ECHO) {
      const bool ok =
          rudp_bench_gns::send_payload(iface_, origin, data, len,
                                       header.flags) == 0;
      count_submit(cls, dist, measured, ok);
      return;
    }

    // broadcast: 現在の全接続(origin 含む)へ無変更で fanout。
    for (const HSteamNetConnection conn : conns_) {
      const bool ok =
          rudp_bench_gns::send_payload(iface_, conn, data, len,
                                       header.flags) == 0;
      count_submit(cls, dist, measured, ok);
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
  HSteamListenSocket listen_sock_ = k_HSteamListenSocket_Invalid;
  HSteamNetPollGroup poll_group_ = k_HSteamNetPollGroup_Invalid;
  std::vector<HSteamNetConnection> conns_;
  ServerStats stats_;
};

void status_callback(SteamNetConnectionStatusChangedCallback_t *info) {
  if (g_app != nullptr) {
    g_app->on_status_changed(info);
  }
}

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
  g_app = app;
  if (!app->start(port)) {
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
      if (app->service() != 0) {
        std::fprintf(stderr, "gns service failed\n");
        bk_control_close(control);
        return EXIT_FAILURE;
      }
      std::this_thread::sleep_for(std::chrono::nanoseconds(kServiceSliceNs));
    }
  }

  while (!g_stop) {
    uint64_t sleep_ns = kServiceSliceNs;
    if (control != nullptr) {
      const uint64_t now = bk_now_ns();
      if (now >= schedule.drain_until_ns) {
        break;
      }
      const uint64_t remain_ns = schedule.drain_until_ns - now;
      if (remain_ns < sleep_ns) {
        sleep_ns = remain_ns;
      }
    }
    if (app->service() != 0) {
      std::fprintf(stderr, "gns service failed\n");
      if (control != nullptr) {
        bk_control_close(control);
      }
      return EXIT_FAILURE;
    }
    std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
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

  app->shutdown();
  return EXIT_SUCCESS;
}
