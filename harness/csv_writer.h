#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>

namespace rudp_bench {

struct CsvRow {
  std::string library;
  std::string encryption;       // "on"/"off"
  int phase = 1;
  uint32_t rate_r = 0;          // reliable msg/s/conn requested
  uint32_t rate_u = 0;          // unreliable msg/s/conn requested
  uint32_t size = 0;
  uint32_t conns = 0;
  double loss = 0.0;
  double throughput_mbps = 0.0;
  uint64_t msg_per_sec = 0;
  uint64_t rtt_r_p50_us = 0;
  uint64_t rtt_r_p95_us = 0;
  uint64_t rtt_r_p99_us = 0;
  uint64_t rtt_u_p50_us = 0;
  uint64_t rtt_u_p95_us = 0;
  uint64_t rtt_u_p99_us = 0;
  uint64_t delivered = 0;
  uint64_t accepted = 0;
  double delivery_ratio = 0.0;
  double cpu_pct = 0.0;
  uint64_t rss_mb = 0;
  uint64_t connect_ms = 0;
  uint32_t duration_s = 0;
  std::string mode = "echo";        // "echo" / "broadcast"
  std::string idle_policy = "spin";  // "spin" / "adaptive"
  std::string flush_policy = "immediate";
  uint64_t client_tick_gap_p99_us = 0;
  uint64_t client_tick_gap_max_us = 0;
  uint64_t client_pacing_lag_p99_us = 0;
  uint64_t client_pacing_lag_max_us = 0;
  uint64_t client_missed_pacing = 0;
  uint64_t client_attempted = 0;
  uint64_t client_accepted = 0;
  double client_attempted_ratio = 0.0;
  double client_accepted_ratio = 0.0;
  uint64_t client_recv_drained_p99 = 0;
  uint64_t client_recv_drained_max = 0;
  uint64_t client_outstanding_max = 0;
  uint32_t client_tick_ok = 0;
  uint32_t conn_peak = 0;            // adapter 観測のピーク同時 connected
  uint32_t conn_disc_transport = 0;  // bench 中に transport で切れた数
  uint32_t conn_disc_peer = 0;       // peer から close 通告された数
  std::string delivery_dedup_policy = "sliding_window_65536_per_conn";
};

void write_header(std::ostream& os);
void write_row(std::ostream& os, const CsvRow& r);

}  // namespace rudp_bench
