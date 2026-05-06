#pragma once

#include <cstdint>
#include <iosfwd>
#include <string>

namespace rudp_bench {

struct CsvRow {
  std::string library;
  std::string encryption;       // "on"/"off"
  int phase = 1;
  std::string reliable;          // "r"/"u"/"na"
  uint32_t size = 0;
  uint32_t conns = 0;
  uint32_t rate = 0;
  double loss = 0.0;
  double throughput_mbps = 0.0;
  uint64_t msg_per_sec = 0;
  uint64_t rtt_p50_us = 0;
  uint64_t rtt_p95_us = 0;
  uint64_t rtt_p99_us = 0;
  uint64_t delivered = 0;
  uint64_t sent = 0;
  double delivery_ratio = 0.0;
  double cpu_pct = 0.0;
  uint64_t rss_mb = 0;
  uint64_t connect_ms = 0;
  uint32_t duration_s = 0;
  std::string mode = "echo";       // "echo" / "broadcast"
};

void write_header(std::ostream& os);
void write_row(std::ostream& os, const CsvRow& r);

}  // namespace rudp_bench
