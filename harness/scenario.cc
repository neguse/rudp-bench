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
    else if (starts_with(a, "--reliable=")) {
      const char* v = value(a, "--reliable=");
      if (std::strcmp(v, "r") == 0) c.reliable = Reliability::Reliable;
      else if (std::strcmp(v, "u") == 0) c.reliable = Reliability::Unreliable;
      else if (std::strcmp(v, "na") == 0) c.reliable = Reliability::NotApplicable;
      else return std::nullopt;
    }
    else if (starts_with(a, "--size=")) c.size_bytes = std::atoi(value(a, "--size="));
    else if (starts_with(a, "--conns=")) c.conns = std::atoi(value(a, "--conns="));
    else if (starts_with(a, "--rate=")) c.rate_per_conn = std::atoi(value(a, "--rate="));
    else if (starts_with(a, "--duration=")) c.duration_s = std::atoi(value(a, "--duration="));
    else if (starts_with(a, "--warmup=")) c.warmup_s = std::atoi(value(a, "--warmup="));
    else if (starts_with(a, "--loss=")) c.loss_pct = std::atof(value(a, "--loss="));
    else if (starts_with(a, "--out=")) c.out_path = value(a, "--out=");
    else {
      std::cerr << "unknown flag: " << a << "\n";
      return std::nullopt;
    }
  }
  if (c.library.empty()) return std::nullopt;
  return c;
}

}  // namespace rudp_bench
