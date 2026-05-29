#include "harness/scenario.h"

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>

namespace rudp_bench {
namespace {

bool starts_with(const char* s, const char* p) {
  return std::strncmp(s, p, std::strlen(p)) == 0;
}

const char* value(const char* s, const char* p) {
  return s + std::strlen(p);
}

bool parse_u64(const char* s, uint64_t max_value, uint64_t* out) {
  if (s == nullptr || *s == '\0' || *s == '-' || *s == '+') return false;
  for (const char* p = s; *p != '\0'; ++p) {
    if (*p < '0' || *p > '9') return false;
  }
  errno = 0;
  char* end = nullptr;
  unsigned long long parsed = std::strtoull(s, &end, 10);
  if (errno == ERANGE || end == s || *end != '\0' || parsed > max_value) {
    return false;
  }
  *out = static_cast<uint64_t>(parsed);
  return true;
}

bool parse_u32(const char* s, uint32_t* out) {
  uint64_t parsed = 0;
  if (!parse_u64(s, std::numeric_limits<uint32_t>::max(), &parsed)) {
    return false;
  }
  *out = static_cast<uint32_t>(parsed);
  return true;
}

bool parse_u16(const char* s, uint16_t* out) {
  uint64_t parsed = 0;
  if (!parse_u64(s, std::numeric_limits<uint16_t>::max(), &parsed)) {
    return false;
  }
  *out = static_cast<uint16_t>(parsed);
  return true;
}

bool parse_loss_pct(const char* s, double* out) {
  if (s == nullptr || *s == '\0') return false;
  errno = 0;
  char* end = nullptr;
  double parsed = std::strtod(s, &end);
  if (errno == ERANGE || end == s || *end != '\0' ||
      !std::isfinite(parsed) || parsed < 0.0 || parsed > 100.0) {
    return false;
  }
  *out = parsed;
  return true;
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
    else if (starts_with(a, "--port=")) {
      if (!parse_u16(value(a, "--port="), &c.port)) return std::nullopt;
    }
    else if (starts_with(a, "--rate-r=")) {
      if (!parse_u32(value(a, "--rate-r="), &c.rate_r)) return std::nullopt;
    }
    else if (starts_with(a, "--rate-u=")) {
      if (!parse_u32(value(a, "--rate-u="), &c.rate_u)) return std::nullopt;
    }
    else if (starts_with(a, "--size=")) {
      if (!parse_u32(value(a, "--size="), &c.size_bytes)) return std::nullopt;
    }
    else if (starts_with(a, "--conns=")) {
      if (!parse_u32(value(a, "--conns="), &c.conns)) return std::nullopt;
    }
    else if (starts_with(a, "--duration=")) {
      if (!parse_u32(value(a, "--duration="), &c.duration_s)) return std::nullopt;
    }
    else if (starts_with(a, "--warmup=")) {
      if (!parse_u32(value(a, "--warmup="), &c.warmup_s)) return std::nullopt;
    }
    else if (starts_with(a, "--ramp-up-ms=")) {
      if (!parse_u32(value(a, "--ramp-up-ms="), &c.ramp_up_ms)) return std::nullopt;
    }
    else if (starts_with(a, "--loss=")) {
      if (!parse_loss_pct(value(a, "--loss="), &c.loss_pct)) return std::nullopt;
    }
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
  if (c.conns == 0) {
    std::cerr << "--conns must be > 0\n";
    return std::nullopt;
  }
  return c;
}

}  // namespace rudp_bench
