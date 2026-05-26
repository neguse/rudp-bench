#include "harness/scenario.h"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace rudp_bench {
namespace {

bool starts_with(const char* s, const char* p) {
  return std::strncmp(s, p, std::strlen(p)) == 0;
}

const char* value(const char* s, const char* p) {
  return s + std::strlen(p);
}

}  // namespace

const char* idle_policy_name(IdlePolicy p) {
  return p == IdlePolicy::Adaptive ? "adaptive" : "spin";
}

std::optional<ScenarioConfig> parse_scenario(int argc, const char* argv[]) {
  ScenarioConfig c;
  for (int i = 1; i < argc; ++i) {
    const char* a = argv[i];
    if (starts_with(a, "--library=")) c.library = value(a, "--library=");
    else if (starts_with(a, "--role=")) {
      const char* v = value(a, "--role=");
      if (std::strcmp(v, "server") == 0) c.role = Role::Server;
      else if (std::strcmp(v, "client") == 0) c.role = Role::Client;
      else return std::nullopt;
    }
    else if (starts_with(a, "--host=")) c.host = value(a, "--host=");
    else if (starts_with(a, "--port=")) c.port = static_cast<uint16_t>(std::atoi(value(a, "--port=")));
    else if (starts_with(a, "--rate-r=")) c.rate_r = std::atoi(value(a, "--rate-r="));
    else if (starts_with(a, "--rate-u=")) c.rate_u = std::atoi(value(a, "--rate-u="));
    else if (starts_with(a, "--size=")) c.size_bytes = std::atoi(value(a, "--size="));
    else if (starts_with(a, "--conns=")) c.conns = std::atoi(value(a, "--conns="));
    else if (starts_with(a, "--duration=")) c.duration_s = std::atoi(value(a, "--duration="));
    else if (starts_with(a, "--warmup=")) c.warmup_s = std::atoi(value(a, "--warmup="));
    else if (starts_with(a, "--loss=")) c.loss_pct = std::atof(value(a, "--loss="));
    else if (starts_with(a, "--mode=")) {
      const char* v = value(a, "--mode=");
      if (std::strcmp(v, "echo") == 0) c.mode = ServerMode::Echo;
      else if (std::strcmp(v, "broadcast") == 0) c.mode = ServerMode::Broadcast;
      else return std::nullopt;
    }
    else if (starts_with(a, "--idle=")) {
      const char* v = value(a, "--idle=");
      if (std::strcmp(v, "spin") == 0) c.idle_policy = IdlePolicy::Spin;
      else if (std::strcmp(v, "adaptive") == 0) c.idle_policy = IdlePolicy::Adaptive;
      else return std::nullopt;
    }
    else if (starts_with(a, "--out=")) c.out_path = value(a, "--out=");
    else if (starts_with(a, "--bins-r-out=")) c.bins_r_out_path = value(a, "--bins-r-out=");
    else if (starts_with(a, "--bins-u-out=")) c.bins_u_out_path = value(a, "--bins-u-out=");
    else {
      std::cerr << "unknown flag: " << a << "\n";
      return std::nullopt;
    }
  }
  if (c.library.empty()) return std::nullopt;
  if (c.rate_r == 0 && c.rate_u == 0) {
    std::cerr << "at least one of --rate-r / --rate-u must be > 0\n";
    return std::nullopt;
  }
  return c;
}

}  // namespace rudp_bench
