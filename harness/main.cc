#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>

#include "harness/adapter_registry.h"
#include "harness/csv_writer.h"
#include "harness/metrics.h"
#include "harness/runner.h"
#include "harness/scenario.h"

namespace rudp_bench {
void register_raw_udp_adapter();
void register_mini_rudp_adapter();
void register_enet_adapter();
void register_kcp_adapter();
void register_slikenet_adapter();
void register_udt4_adapter();
void register_yojimbo_adapter();
void register_gns_adapter();
void register_msquic_adapter();
}  // namespace rudp_bench

int main(int argc, const char* argv[]) {
  rudp_bench::register_raw_udp_adapter();
  rudp_bench::register_mini_rudp_adapter();
  rudp_bench::register_enet_adapter();
  rudp_bench::register_kcp_adapter();
  rudp_bench::register_slikenet_adapter();
  rudp_bench::register_udt4_adapter();
  rudp_bench::register_yojimbo_adapter();
  rudp_bench::register_gns_adapter();
  rudp_bench::register_msquic_adapter();

  auto cfg_opt = rudp_bench::parse_scenario(argc, argv);
  if (!cfg_opt) {
    std::cerr << "usage: rudp-bench --library=<name> --role=server|client "
                 "--rate-r=<hz> --rate-u=<hz> "
                 "[--idle=spin|adaptive] ...\n";
    return 2;
  }
  auto& cfg = *cfg_opt;

  auto adapter = rudp_bench::create_adapter(cfg.library);
  if (!adapter) {
    std::cerr << "unknown library: " << cfg.library << "\n";
    return 2;
  }

  // For flush_policy emission we prefer the reliable side when present,
  // otherwise the unreliable side.
  const bool reliable_active = cfg.rate_r > 0;

  auto make_skipped_row = [&]() {
    rudp_bench::CsvRow row;
    row.library = cfg.library;
    row.encryption = adapter->encryption_on() ? "on" : "off";
    row.rate_r = cfg.rate_r;
    row.rate_u = cfg.rate_u;
    row.size = cfg.size_bytes;
    row.conns = cfg.conns;
    row.loss = cfg.loss_pct;
    row.duration_s = cfg.duration_s;
    row.mode = (cfg.mode == rudp_bench::ServerMode::Broadcast) ? "broadcast" : "echo";
    row.idle_policy = rudp_bench::idle_policy_name(cfg.idle_policy);
    row.flush_policy = adapter->flush_policy(reliable_active);
    row.delivery_dedup_policy = rudp_bench::DeliveryTracker::dedup_policy();
    return row;
  };

  auto write_output = [&](const rudp_bench::CsvRow& row) {
    if (!cfg.out_path.empty()) {
      std::ofstream f(cfg.out_path);
      rudp_bench::write_header(f);
      rudp_bench::write_row(f, row);
    } else {
      rudp_bench::write_header(std::cout);
      rudp_bench::write_row(std::cout, row);
    }
  };

  // Capability check: each requested channel must be supported by the adapter.
  // Reducer interprets a skipped row by re-applying capability rules from
  // capabilities.py, so the harness only needs to short-circuit (no `na` flag
  // in the row schema anymore).
  {
    if (cfg.rate_r > 0 && !adapter->supports(true)) {
      std::cerr << "library " << cfg.library
                << " does not support reliable; emit skipped row\n";
      write_output(make_skipped_row());
      return 0;
    }
    if (cfg.rate_u > 0 && !adapter->supports(false)) {
      std::cerr << "library " << cfg.library
                << " does not support unreliable; emit skipped row\n";
      write_output(make_skipped_row());
      return 0;
    }
    const size_t min_payload = 17;  // 8B seq + 8B ts + 1B reliable flag
    size_t max_payload = std::numeric_limits<size_t>::max();
    if (cfg.rate_r > 0) {
      max_payload = std::min(max_payload, adapter->max_payload_bytes(true));
    }
    if (cfg.rate_u > 0) {
      max_payload = std::min(max_payload, adapter->max_payload_bytes(false));
    }
    if (cfg.size_bytes < min_payload || cfg.size_bytes > max_payload) {
      std::cerr << "library " << cfg.library << " supports payload size "
                << min_payload << ".." << max_payload << " bytes; emit skipped row\n";
      write_output(make_skipped_row());
      return 0;
    }
    const uint32_t max_conns = adapter->max_connections();
    if (cfg.conns > max_conns) {
      std::cerr << "library " << cfg.library << " supports up to "
                << max_conns << " connections; emit skipped row\n";
      write_output(make_skipped_row());
      return 0;
    }
  }

  rudp_bench::CsvRow row =
      cfg.role == rudp_bench::Role::Server
          ? rudp_bench::run_server(*adapter, cfg)
          : rudp_bench::run_client(*adapter, cfg);

  write_output(row);
  // msquic workers can still hold callback contexts pointing into the
  // adapter when main returns; the C++ destructor chain then UAFs into
  // them and trips glibc's double-free check. Skip destructors entirely
  // and let the OS reclaim memory at process exit.
  if (cfg.library == "msquic") {
    std::_Exit(0);
  }
  return 0;
}
