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
  uint64_t delivered_r = 0;
  uint64_t delivered_u = 0;
  uint64_t accepted_r = 0;
  uint64_t accepted_u = 0;
  double delivery_ratio = 0.0;
  uint64_t server_received = 0;       // measured client->server messages
  uint64_t server_echo_accepted = 0;  // measured server echo sends accepted
  uint64_t server_received_r = 0;
  uint64_t server_received_u = 0;
  uint64_t server_echo_accepted_r = 0;
  uint64_t server_echo_accepted_u = 0;
  uint64_t server_recv_drained_p99 = 0;
  uint64_t server_recv_drained_max = 0;
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
  double cpu_pct_peak = 0.0;         // periodic-sampled peak CPU% (M1)
  uint64_t close_ms = 0;             // teardown(close()) 所要時間 ms (L6)
  // §5.3: coordinated omission 診断。スケジュール時刻（sched.next_send）基準の
  // corrected RTT。実送信時刻基準の rtt_* に対し、送信ループの停滞
  // （pacing lag）ぶんだけ大きく出る。差が大きい run は送信側が飽和している。
  uint64_t rtt_sched_r_p50_us = 0;
  uint64_t rtt_sched_r_p95_us = 0;
  uint64_t rtt_sched_r_p99_us = 0;
  uint64_t rtt_sched_u_p50_us = 0;
  uint64_t rtt_sched_u_p95_us = 0;
  uint64_t rtt_sched_u_p99_us = 0;
  // §6.2: adapter 内 inbound queue が上限到達で落とした受信メッセージ累計
  // （プロセス全体、ReusableInboundQueue の drop 合計）。delivery 低下の
  // 切り分け用診断値。
  uint64_t inbox_dropped = 0;
  // §3.2-3.3: 公平性メタデータ。実効 CC アルゴリズムとスレッドモデル。
  std::string cc_algo = "unknown";
  std::string thread_model = "unknown";
};

void write_header(std::ostream& os);
void write_row(std::ostream& os, const CsvRow& r);

}  // namespace rudp_bench
