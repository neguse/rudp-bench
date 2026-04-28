#include <fstream>
#include <iostream>

#include "harness/adapter_registry.h"
#include "harness/csv_writer.h"
#include "harness/runner.h"
#include "harness/scenario.h"

namespace rudp_bench {
void register_raw_udp_adapter();
void register_mini_rudp_adapter();
void register_enet_adapter();
void register_kcp_adapter();
}  // namespace rudp_bench

int main(int argc, const char* argv[]) {
  rudp_bench::register_raw_udp_adapter();
  rudp_bench::register_mini_rudp_adapter();
  rudp_bench::register_enet_adapter();
  rudp_bench::register_kcp_adapter();

  auto cfg_opt = rudp_bench::parse_scenario(argc, argv);
  if (!cfg_opt) {
    std::cerr << "usage: rudp-bench --library=<name> --role=server|client ...\n";
    return 2;
  }
  auto& cfg = *cfg_opt;

  auto adapter = rudp_bench::create_adapter(cfg.library);
  if (!adapter) {
    std::cerr << "unknown library: " << cfg.library << "\n";
    return 2;
  }

  // capability check
  if (cfg.reliable == rudp_bench::Reliability::Reliable && !adapter->supports(true)) {
    std::cerr << "library " << cfg.library << " does not support reliable; emit na row\n";
    rudp_bench::CsvRow row;
    row.library = cfg.library;
    row.encryption = adapter->encryption_on() ? "on" : "off";
    row.reliable = "na";
    row.size = cfg.size_bytes;
    row.conns = cfg.conns;
    row.rate = cfg.rate_per_conn;
    row.loss = cfg.loss_pct;
    row.duration_s = cfg.duration_s;
    if (!cfg.out_path.empty()) {
      std::ofstream f(cfg.out_path);
      rudp_bench::write_header(f);
      rudp_bench::write_row(f, row);
    } else {
      rudp_bench::write_header(std::cout);
      rudp_bench::write_row(std::cout, row);
    }
    return 0;
  }

  rudp_bench::CsvRow row =
      cfg.role == rudp_bench::Role::Server
          ? rudp_bench::run_server(*adapter, cfg)
          : rudp_bench::run_client(*adapter, cfg);

  if (!cfg.out_path.empty()) {
    std::ofstream f(cfg.out_path);
    rudp_bench::write_header(f);
    rudp_bench::write_row(f, row);
  } else {
    rudp_bench::write_header(std::cout);
    rudp_bench::write_row(std::cout, row);
  }
  return 0;
}
