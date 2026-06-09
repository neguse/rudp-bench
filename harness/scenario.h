#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace rudp_bench {

enum class Role { Server, Client };
enum class ServerMode { Echo, Broadcast };
enum class IdlePolicy { Spin, Adaptive };

struct ScenarioConfig {
  std::string library;
  Role role = Role::Client;
  std::string host = "127.0.0.1";
  uint16_t port = 9000;
  uint32_t rate_r = 0;            // reliable msg/s per conn (0 = no reliable traffic)
  uint32_t rate_u = 0;            // unreliable msg/s per conn (0 = no unreliable traffic)
  uint32_t size_bytes = 64;
  uint32_t conns = 1;
  uint32_t fanout_conns = 0;      // broadcast denominator; 0 means conns
  uint32_t conn_id_offset = 0;    // global origin id offset for split clients
  uint32_t duration_s = 30;
  uint32_t warmup_s = 2;
  // 接続確立を一気にせず一定時間に分散させる ramp-up 時間。conns 個の connect を
  // 等間隔で発行するので、各 connect の間隔は ramp_up_ms / conns。lib によっては
  // 全 conn 同時 handshake が listener / TLS スタックで詰まるため、200conn では
  // ~10s 程度確保した方が安定する。
  uint32_t ramp_up_ms = 0;
  // Active send window が終わった後、client が poll/recv を続ける時間。
  // loss 下の reliable retransmit は 500ms を超えて戻ることがあるため、
  // scenario metadata として残し、必要に応じて延ばせるようにする。
  uint32_t tail_ms = 500;
  double loss_pct = 0.0;          // メタデータ(tc は外側で設定済み前提)
  ServerMode mode = ServerMode::Echo;  // echo: 1:1 / broadcast: 1:N (全 conn)
  IdlePolicy idle_policy = IdlePolicy::Spin;
  std::string out_path;
  // Optional sidecar files for cross-process histogram merge. When set, the
  // client role writes the dense bin contents of rtt_r / rtt_u so a coordinator
  // can sum N clients' bins and recompute percentiles correctly.
  std::string bins_r_out_path;
  std::string bins_u_out_path;
};

const char* idle_policy_name(IdlePolicy p);
std::optional<ScenarioConfig> parse_scenario(int argc, const char* argv[]);

}  // namespace rudp_bench
