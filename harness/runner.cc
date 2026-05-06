#include "harness/runner.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <unordered_set>
#include <vector>

#include "harness/metrics.h"
#include "harness/proc_sampler.h"

namespace rudp_bench {

CsvRow run_server(Adapter& a, const ScenarioConfig& cfg) {
  ProcSampler ps;
  a.server_listen(cfg.port);
  ps.begin();

  std::vector<uint8_t> buf(65536);
  std::unordered_set<uint32_t> known_conns;  // broadcast 配信先
  bool reliable = (cfg.reliable == Reliability::Reliable);
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::seconds(cfg.duration_s + cfg.warmup_s + 5);
  while (std::chrono::steady_clock::now() < deadline) {
    a.poll();
    size_t n; uint32_t cid;
    int r = a.recv(buf.data(), buf.size(), &n, &cid);
    if (r == 1) {
      known_conns.insert(cid);
      if (cfg.mode == ServerMode::Echo) {
        a.send(cid, buf.data(), n, reliable);
      } else {
        // Broadcast: 既知の全 conn に同 payload を送る(送信元を含む)
        for (uint32_t target : known_conns) {
          a.send(target, buf.data(), n, reliable);
        }
      }
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
  row.mode = (cfg.mode == ServerMode::Broadcast) ? "broadcast" : "echo";
  return row;
}

CsvRow run_client(Adapter& a, const ScenarioConfig& cfg) {
  using clock = std::chrono::steady_clock;
  ProcSampler ps;
  LatencyHist rtt;
  ThroughputCounter th;
  DeliveryTracker dt;

  // connect all
  std::vector<uint32_t> ids;
  ids.reserve(cfg.conns);
  auto t_connect_begin = clock::now();
  for (uint32_t i = 0; i < cfg.conns; ++i) {
    ids.push_back(a.client_connect(cfg.host.c_str(), cfg.port));
  }
  // 全コネクションが ready になるまで poll
  for (auto id : ids) {
    while (!a.is_connected(id)) {
      a.poll();
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
  }
  auto t_connect_end = clock::now();
  uint64_t connect_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            t_connect_end - t_connect_begin).count();

  ps.begin();
  auto warmup_end = clock::now() + std::chrono::seconds(cfg.warmup_s);
  auto run_end = warmup_end + std::chrono::seconds(cfg.duration_s);

  // pacing: 各 conn で次回送信時刻を保持
  std::vector<clock::time_point> next_send(cfg.conns, clock::now());
  std::chrono::nanoseconds interval =
      cfg.rate_per_conn ? std::chrono::nanoseconds(1'000'000'000ULL / cfg.rate_per_conn)
                        : std::chrono::nanoseconds(0);

  std::vector<uint8_t> payload(cfg.size_bytes, 0xAB);
  std::vector<uint64_t> seq_counter(cfg.conns, 1);
  std::vector<uint8_t> rxbuf(65536);

  bool reliable = (cfg.reliable == Reliability::Reliable);
  auto in_measure = [&](clock::time_point t) { return t >= warmup_end; };

  // tail drain: 送信終了後しばらく recv だけ
  auto tail_until = run_end + std::chrono::milliseconds(500);

  while (clock::now() < tail_until) {
    auto now = clock::now();
    if (now < run_end) {
      for (uint32_t i = 0; i < cfg.conns; ++i) {
        if (now < next_send[i]) continue;
        // ヘッダ書き込み: seq は (src_idx << 32 | local) でグローバルにユニーク化
        // (broadcast 時の dedup key が src 違いで衝突しないため)
        uint64_t local_seq = seq_counter[i]++;
        uint64_t global_seq = (static_cast<uint64_t>(i) << 32) | local_seq;
        uint64_t ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          now.time_since_epoch()).count();
        std::memcpy(payload.data(), &global_seq, 8);
        std::memcpy(payload.data() + 8, &ts, 8);
        if (a.send(ids[i], payload.data(), payload.size(), reliable) == 0) {
          if (in_measure(now)) {
            // echo: 期待受信 1, broadcast: 期待受信 cfg.conns
            uint32_t expected = (cfg.mode == ServerMode::Broadcast) ? cfg.conns : 1;
            for (uint32_t k = 0; k < expected; ++k) {
              dt.mark_sent(global_seq, ids[i]);
            }
          }
        }
        if (cfg.rate_per_conn) {
          next_send[i] += interval;
          if (next_send[i] < now) next_send[i] = now;  // catch-up cap
        }
      }
    }
    a.poll();
    while (true) {
      size_t n; uint32_t cid;
      int r = a.recv(rxbuf.data(), rxbuf.size(), &n, &cid);
      if (r != 1) break;
      if (n < 16) continue;
      uint64_t seq, ts;
      std::memcpy(&seq, rxbuf.data(), 8);
      std::memcpy(&ts, rxbuf.data() + 8, 8);
      auto t_recv = clock::now();
      uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            t_recv.time_since_epoch()).count();
      uint64_t rtt_us = (now_ns - ts) / 1000;
      if (in_measure(t_recv)) {
        rtt.record_us(rtt_us);
        th.record(n);
        dt.mark_received(seq, cid);
      }
    }
  }
  ps.end();
  a.close();

  CsvRow row;
  row.library = cfg.library;
  row.encryption = a.encryption_on() ? "on" : "off";
  row.phase = 1;
  row.reliable = reliable ? "r"
                          : (cfg.reliable == Reliability::Unreliable ? "u" : "na");
  row.size = cfg.size_bytes;
  row.conns = cfg.conns;
  row.rate = cfg.rate_per_conn;
  row.loss = cfg.loss_pct;
  row.throughput_mbps = (th.bytes() * 8.0) / (cfg.duration_s * 1'000'000.0);
  row.msg_per_sec = th.messages() / std::max<uint32_t>(1, cfg.duration_s);
  row.rtt_p50_us = rtt.percentile_us(0.50);
  row.rtt_p95_us = rtt.percentile_us(0.95);
  row.rtt_p99_us = rtt.percentile_us(0.99);
  row.delivered = dt.received();
  row.sent = dt.sent();
  row.delivery_ratio = dt.delivery_ratio();
  row.cpu_pct = ps.cpu_pct();
  row.rss_mb = ps.rss_max_mb();
  row.connect_ms = connect_ms;
  row.duration_s = cfg.duration_s;
  row.mode = (cfg.mode == ServerMode::Broadcast) ? "broadcast" : "echo";
  return row;
}

}  // namespace rudp_bench
