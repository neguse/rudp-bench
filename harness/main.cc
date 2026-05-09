#include <fstream>
#include <iostream>
#include <string>

#include "harness/adapter_registry.h"
#include "harness/csv_writer.h"
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
                 "[--idle=spin|adaptive] ...\n";
    return 2;
  }
  auto& cfg = *cfg_opt;

  auto adapter = rudp_bench::create_adapter(cfg.library);
  if (!adapter) {
    std::cerr << "unknown library: " << cfg.library << "\n";
    return 2;
  }

  auto make_skipped_row = [&](const std::string& reliable) {
    rudp_bench::CsvRow row;
    row.library = cfg.library;
    row.encryption = adapter->encryption_on() ? "on" : "off";
    row.reliable = reliable;
    row.size = cfg.size_bytes;
    row.conns = cfg.conns;
    row.rate = cfg.rate_per_conn;
    row.loss = cfg.loss_pct;
    row.duration_s = cfg.duration_s;
    row.mode = (cfg.mode == rudp_bench::ServerMode::Broadcast) ? "broadcast" : "echo";
    row.idle_policy = rudp_bench::idle_policy_name(cfg.idle_policy);
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

  // capability check: emit na row if the requested mode is unsupported
  {
    bool want_reliable = (cfg.reliable == rudp_bench::Reliability::Reliable);
    if (!adapter->supports(want_reliable)) {
      std::cerr << "library " << cfg.library
                << (want_reliable ? " does not support reliable"
                                  : " does not support unreliable")
                << "; emit na row\n";
      write_output(make_skipped_row("na"));
      return 0;
    }
    const size_t min_payload = 16;
    const size_t max_payload = adapter->max_payload_bytes(want_reliable);
    if (cfg.size_bytes < min_payload || cfg.size_bytes > max_payload) {
      std::cerr << "library " << cfg.library << " supports payload size "
                << min_payload << ".." << max_payload << " bytes; emit skipped row\n";
      write_output(make_skipped_row(want_reliable ? "r" : "u"));
      return 0;
    }
    const uint32_t max_conns = adapter->max_connections();
    if (cfg.conns > max_conns) {
      std::cerr << "library " << cfg.library << " supports up to "
                << max_conns << " connections; emit skipped row\n";
      write_output(make_skipped_row(want_reliable ? "r" : "u"));
      return 0;
    }
  }

  rudp_bench::CsvRow row =
      cfg.role == rudp_bench::Role::Server
          ? rudp_bench::run_server(*adapter, cfg)
          : rudp_bench::run_client(*adapter, cfg);

  write_output(row);
  return 0;
}
