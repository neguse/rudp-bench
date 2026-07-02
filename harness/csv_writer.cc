#include "harness/csv_writer.h"

#include <iomanip>
#include <ostream>

namespace rudp_bench {

void write_header(std::ostream& os) {
  os << "library,encryption,phase,rate_r,rate_u,size,conns,loss,"
     << "throughput_mbps,msg_per_sec,"
     << "rtt_r_p50_us,rtt_r_p95_us,rtt_r_p99_us,"
     << "rtt_u_p50_us,rtt_u_p95_us,rtt_u_p99_us,"
     << "delivered,accepted,delivered_r,delivered_u,accepted_r,accepted_u,"
     << "delivery_ratio,"
     << "server_received,server_echo_accepted,"
     << "server_received_r,server_received_u,"
     << "server_echo_accepted_r,server_echo_accepted_u,"
     << "server_recv_drained_p99,server_recv_drained_max,"
     << "cpu_pct,rss_mb,connect_ms,duration_s,"
     << "mode,idle_policy,flush_policy,client_tick_gap_p99_us,"
     << "client_tick_gap_max_us,"
     << "client_pacing_lag_p99_us,client_pacing_lag_max_us,"
     << "client_missed_pacing,client_attempted,client_accepted,"
     << "client_attempted_ratio,client_accepted_ratio,"
     << "client_recv_drained_p99,client_recv_drained_max,"
     << "client_outstanding_max,client_tick_ok,"
     << "conn_peak,conn_disc_transport,conn_disc_peer,"
     << "delivery_dedup_policy,cpu_pct_peak,close_ms,"
     // 新規診断列は末尾追加のみ（Go 側リーダーはヘッダ名ベース。既存列の
     // 削除・改名・順序変更は互換性破壊なので禁止）。
     << "rtt_sched_r_p50_us,rtt_sched_r_p95_us,rtt_sched_r_p99_us,"
     << "rtt_sched_u_p50_us,rtt_sched_u_p95_us,rtt_sched_u_p99_us,"
     << "inbox_dropped,cc_algo,thread_model\n";
}

void write_row(std::ostream& os, const CsvRow& r) {
  os << r.library << ',' << r.encryption << ',' << r.phase << ','
     << r.rate_r << ',' << r.rate_u << ','
     << r.size << ',' << r.conns << ','
     << std::fixed << std::setprecision(3) << r.loss << ','
     << std::setprecision(3) << r.throughput_mbps << ','
     << r.msg_per_sec << ','
     << r.rtt_r_p50_us << ',' << r.rtt_r_p95_us << ',' << r.rtt_r_p99_us << ','
     << r.rtt_u_p50_us << ',' << r.rtt_u_p95_us << ',' << r.rtt_u_p99_us << ','
     << r.delivered << ',' << r.accepted << ','
     << r.delivered_r << ',' << r.delivered_u << ','
     << r.accepted_r << ',' << r.accepted_u << ','
     << std::setprecision(4) << r.delivery_ratio << ','
     << r.server_received << ',' << r.server_echo_accepted << ','
     << r.server_received_r << ',' << r.server_received_u << ','
     << r.server_echo_accepted_r << ',' << r.server_echo_accepted_u << ','
     << r.server_recv_drained_p99 << ',' << r.server_recv_drained_max << ','
     << std::setprecision(2) << r.cpu_pct << ','
     << r.rss_mb << ',' << r.connect_ms << ',' << r.duration_s << ','
     << r.mode << ',' << r.idle_policy << ',' << r.flush_policy << ','
     << r.client_tick_gap_p99_us << ',' << r.client_tick_gap_max_us << ','
     << r.client_pacing_lag_p99_us << ',' << r.client_pacing_lag_max_us << ','
     << r.client_missed_pacing << ',' << r.client_attempted << ','
     << r.client_accepted << ','
     << std::setprecision(4) << r.client_attempted_ratio << ','
     << r.client_accepted_ratio << ','
     << r.client_recv_drained_p99 << ',' << r.client_recv_drained_max << ','
     << r.client_outstanding_max << ',' << r.client_tick_ok << ','
     << r.conn_peak << ',' << r.conn_disc_transport << ','
     << r.conn_disc_peer << ','
     << r.delivery_dedup_policy << ','
     << std::setprecision(2) << r.cpu_pct_peak << ',' << r.close_ms << ','
     << r.rtt_sched_r_p50_us << ',' << r.rtt_sched_r_p95_us << ','
     << r.rtt_sched_r_p99_us << ','
     << r.rtt_sched_u_p50_us << ',' << r.rtt_sched_u_p95_us << ','
     << r.rtt_sched_u_p99_us << ','
     << r.inbox_dropped << ','
     << r.cc_algo << ',' << r.thread_model << '\n';
}

}  // namespace rudp_bench
