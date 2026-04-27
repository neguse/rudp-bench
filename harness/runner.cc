#include "harness/runner.h"

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "harness/metrics.h"
#include "harness/proc_sampler.h"

namespace rudp_bench {

CsvRow run_server(Adapter& a, const ScenarioConfig& cfg) {
  ProcSampler ps;
  a.server_listen(cfg.port);
  ps.begin();

  std::vector<uint8_t> buf(65536);
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::seconds(cfg.duration_s + cfg.warmup_s + 5);
  while (std::chrono::steady_clock::now() < deadline) {
    a.poll();
    size_t n; uint32_t cid;
    int r = a.recv(buf.data(), buf.size(), &n, &cid);
    if (r == 1) {
      bool reliable = (cfg.reliable == Reliability::Reliable);
      a.send(cid, buf.data(), n, reliable);
    } else {
      std::this_thread::sleep_for(std::chrono::microseconds(50));
    }
  }
  ps.end();
  a.close();

  CsvRow row;
  row.library = cfg.library;
  row.encryption = a.encryption_on() ? "on" : "off";
  row.phase = 1;
  row.reliable = cfg.reliable == Reliability::Reliable ? "r" :
                 cfg.reliable == Reliability::Unreliable ? "u" : "na";
  row.size = cfg.size_bytes;
  row.conns = cfg.conns;
  row.rate = cfg.rate_per_conn;
  row.loss = cfg.loss_pct;
  row.cpu_pct = ps.cpu_pct();
  row.rss_mb = ps.rss_max_mb();
  row.duration_s = cfg.duration_s;
  return row;
}

// client は次のタスクで実装
CsvRow run_client(Adapter&, const ScenarioConfig&) { return {}; }

}  // namespace rudp_bench
