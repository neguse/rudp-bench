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
  uint32_t duration_s = 30;
  uint32_t warmup_s = 2;
  double loss_pct = 0.0;          // メタデータ(tc は外側で設定済み前提)
  ServerMode mode = ServerMode::Echo;  // echo: 1:1 / broadcast: 1:N (全 conn)
  IdlePolicy idle_policy = IdlePolicy::Spin;
  std::string out_path;
};

const char* idle_policy_name(IdlePolicy p);
std::optional<ScenarioConfig> parse_scenario(int argc, const char* argv[]);

}  // namespace rudp_bench
