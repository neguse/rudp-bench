#include "harness/csv_writer.h"

#include <iomanip>
#include <ostream>

namespace rudp_bench {

void write_header(std::ostream& os) {
  os << "library,encryption,phase,reliable,size,conns,rate,loss,"
     << "throughput_mbps,msg_per_sec,rtt_p50_us,rtt_p95_us,rtt_p99_us,"
     << "delivered,sent,delivery_ratio,cpu_pct,rss_mb,connect_ms,duration_s,"
     << "mode,client_tick_gap_p99_us,client_tick_gap_max_us,"
     << "client_pacing_lag_p99_us,client_pacing_lag_max_us,"
     << "client_missed_pacing,client_offered,client_accepted,"
     << "client_offered_ratio,client_accepted_ratio,"
     << "client_recv_drained_p99,client_recv_drained_max,"
     << "client_outstanding_max,client_tick_ok\n";
}

void write_row(std::ostream& os, const CsvRow& r) {
  os << r.library << ',' << r.encryption << ',' << r.phase << ',' << r.reliable << ','
     << r.size << ',' << r.conns << ',' << r.rate << ','
     << std::fixed << std::setprecision(3) << r.loss << ','
     << std::setprecision(3) << r.throughput_mbps << ','
     << r.msg_per_sec << ','
     << r.rtt_p50_us << ',' << r.rtt_p95_us << ',' << r.rtt_p99_us << ','
     << r.delivered << ',' << r.sent << ','
     << std::setprecision(4) << r.delivery_ratio << ','
     << std::setprecision(2) << r.cpu_pct << ','
     << r.rss_mb << ',' << r.connect_ms << ',' << r.duration_s << ','
     << r.mode << ','
     << r.client_tick_gap_p99_us << ',' << r.client_tick_gap_max_us << ','
     << r.client_pacing_lag_p99_us << ',' << r.client_pacing_lag_max_us << ','
     << r.client_missed_pacing << ',' << r.client_offered << ','
     << r.client_accepted << ','
     << std::setprecision(4) << r.client_offered_ratio << ','
     << r.client_accepted_ratio << ','
     << r.client_recv_drained_p99 << ',' << r.client_recv_drained_max << ','
     << r.client_outstanding_max << ',' << r.client_tick_ok << '\n';
}

}  // namespace rudp_bench
