#include <gtest/gtest.h>
#include "harness/csv_writer.h"

#include <fstream>
#include <sstream>

using namespace rudp_bench;

TEST(CsvWriter, WritesHeaderAndRow) {
  CsvRow r;
  r.library = "raw_udp";
  r.encryption = "off";
  r.phase = 1;
  r.reliable = "u";
  r.size = 64;
  r.conns = 1;
  r.rate = 100;
  r.loss = 0.0;
  r.throughput_mbps = 12.5;
  r.msg_per_sec = 24414;
  r.rtt_p50_us = 25;
  r.rtt_p95_us = 80;
  r.rtt_p99_us = 200;
  r.delivered = 1000;
  r.accepted = 1000;
  r.delivery_ratio = 1.0;
  r.cpu_pct = 5.5;
  r.rss_mb = 12;
  r.connect_ms = 0;
  r.duration_s = 30;
  r.idle_policy = "spin";

  std::ostringstream os;
  write_header(os);
  write_row(os, r);

  std::string out = os.str();
  EXPECT_NE(out.find("library,encryption,phase,reliable"), std::string::npos);
  EXPECT_NE(out.find("client_tick_gap_p99_us"), std::string::npos);
  EXPECT_NE(out.find("mode,idle_policy"), std::string::npos);
  EXPECT_NE(out.find("raw_udp,off,1,u,64,1,100,0.000"), std::string::npos);
}
