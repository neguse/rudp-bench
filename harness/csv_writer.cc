#include "harness/csv_writer.h"

#include <iomanip>
#include <ostream>

namespace rudp_bench {

void write_header(std::ostream& os) {
  os << "library,encryption,phase,reliable,size,conns,rate,loss,"
     << "throughput_mbps,msg_per_sec,rtt_p50_us,rtt_p95_us,rtt_p99_us,"
     << "delivered,sent,delivery_ratio,cpu_pct,rss_mb,connect_ms,duration_s,"
     << "mode\n";
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
     << r.mode << '\n';
}

}  // namespace rudp_bench
