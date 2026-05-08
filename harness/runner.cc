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
namespace {

class BoundedHistogram {
 public:
  explicit BoundedHistogram(size_t max_exact) : bins_(max_exact + 1, 0) {}

  void record(uint64_t value) {
    ++count_;
    max_ = std::max(max_, value);
    if (value < bins_.size()) {
      ++bins_[value];
    } else {
      ++overflow_;
    }
  }

  uint64_t percentile_per_mille(uint32_t per_mille) const {
    if (count_ == 0) return 0;
    uint64_t target = (count_ * per_mille + 999) / 1000;
    uint64_t seen = 0;
    for (size_t i = 0; i < bins_.size(); ++i) {
      seen += bins_[i];
      if (seen >= target) return static_cast<uint64_t>(i);
    }
    (void)overflow_;
    return max_;
  }

  uint64_t max() const { return max_; }
  uint64_t count() const { return count_; }

 private:
  std::vector<uint64_t> bins_;
  uint64_t count_ = 0;
  uint64_t max_ = 0;
  uint64_t overflow_ = 0;
};

struct ClientTickStats {
  BoundedHistogram tick_gap_us{10'000};
  BoundedHistogram pacing_lag_us{10'000};
  BoundedHistogram recv_drained{10'000};
  uint64_t missed_pacing = 0;
  uint64_t offered = 0;
  uint64_t accepted = 0;
  uint64_t outstanding_max = 0;
  bool have_last_tick = false;
  std::chrono::steady_clock::time_point last_tick{};

  void record_tick(std::chrono::steady_clock::time_point now, bool in_diag) {
    if (have_last_tick && in_diag) {
      auto gap = std::chrono::duration_cast<std::chrono::microseconds>(
                     now - last_tick)
                     .count();
      tick_gap_us.record(gap > 0 ? static_cast<uint64_t>(gap) : 0);
    }
    last_tick = now;
    have_last_tick = true;
  }

  void record_send_due(uint64_t lag_us, uint64_t missed_budget_us) {
    ++offered;
    pacing_lag_us.record(lag_us);
    if (lag_us > missed_budget_us) ++missed_pacing;
  }

  void record_accepted() { ++accepted; }

  void record_recv_drained(uint64_t n) { recv_drained.record(n); }

  void record_outstanding(uint64_t n) {
    outstanding_max = std::max(outstanding_max, n);
  }
};

uint64_t pacing_budget_us(uint32_t rate_per_conn) {
  if (rate_per_conn == 0) return 0;
  uint64_t interval_us = 1'000'000ULL / rate_per_conn;
  return std::max<uint64_t>(20, std::min<uint64_t>(100, interval_us / 10));
}

}  // namespace

CsvRow run_server(Adapter& a, const ScenarioConfig& cfg) {
  ProcSampler ps;
  a.server_listen(cfg.port);
  ps.begin();

  std::vector<uint8_t> buf(65536);
  std::unordered_set<uint32_t> known_conns;  // broadcast 配信先
  bool reliable = (cfg.reliable == Reliability::Reliable);
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::seconds(cfg.duration_s + cfg.warmup_s + 2);
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
  ClientTickStats tick;

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
  uint64_t outstanding = 0;
  uint64_t target_offered =
      static_cast<uint64_t>(cfg.rate_per_conn) * cfg.conns * cfg.duration_s;
  uint64_t missed_budget = pacing_budget_us(cfg.rate_per_conn);

  bool reliable = (cfg.reliable == Reliability::Reliable);
  auto in_measure = [&](clock::time_point t) { return t >= warmup_end; };

  // tail drain: 送信終了後しばらく recv だけ
  auto tail_until = run_end + std::chrono::milliseconds(500);

  while (clock::now() < tail_until) {
    auto now = clock::now();
    bool in_active_send = now >= warmup_end && now < run_end;
    bool in_diag = now >= warmup_end;
    tick.record_tick(now, in_diag);
    if (now < run_end) {
      for (uint32_t i = 0; i < cfg.conns; ++i) {
        if (now < next_send[i]) continue;
        uint64_t lag_us = 0;
        if (cfg.rate_per_conn && now > next_send[i]) {
          lag_us = static_cast<uint64_t>(
              std::chrono::duration_cast<std::chrono::microseconds>(
                  now - next_send[i])
                  .count());
        }
        if (in_active_send) tick.record_send_due(lag_us, missed_budget);
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
            tick.record_accepted();
            // echo: 期待受信 1, broadcast: 期待受信 cfg.conns
            uint32_t expected = (cfg.mode == ServerMode::Broadcast) ? cfg.conns : 1;
            for (uint32_t k = 0; k < expected; ++k) {
              dt.mark_sent(global_seq, ids[i]);
            }
            outstanding += expected;
            tick.record_outstanding(outstanding);
          }
        }
        if (cfg.rate_per_conn) {
          next_send[i] += interval;
          if (next_send[i] < now) next_send[i] = now;  // catch-up cap
        }
      }
    }
    a.poll();
    uint64_t drained_this_tick = 0;
    while (true) {
      size_t n; uint32_t cid;
      int r = a.recv(rxbuf.data(), rxbuf.size(), &n, &cid);
      if (r != 1) break;
      ++drained_this_tick;
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
        if (dt.mark_received(seq, cid) && outstanding > 0) --outstanding;
      }
    }
    if (in_diag && drained_this_tick > 0) tick.record_recv_drained(drained_this_tick);
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
  row.client_tick_gap_p99_us = tick.tick_gap_us.percentile_per_mille(990);
  row.client_tick_gap_max_us = tick.tick_gap_us.max();
  row.client_pacing_lag_p99_us = tick.pacing_lag_us.percentile_per_mille(990);
  row.client_pacing_lag_max_us = tick.pacing_lag_us.max();
  row.client_missed_pacing = tick.missed_pacing;
  row.client_offered = tick.offered;
  row.client_accepted = tick.accepted;
  row.client_offered_ratio =
      target_offered ? static_cast<double>(tick.offered) /
                           static_cast<double>(target_offered)
                     : 1.0;
  row.client_accepted_ratio =
      tick.offered ? static_cast<double>(tick.accepted) /
                         static_cast<double>(tick.offered)
                   : 0.0;
  row.client_recv_drained_p99 = tick.recv_drained.percentile_per_mille(990);
  row.client_recv_drained_max = tick.recv_drained.max();
  row.client_outstanding_max = tick.outstanding_max;
  bool tick_ok = tick.tick_gap_us.count() > 0 &&
                 row.client_tick_gap_p99_us <= 250 &&
                 row.client_accepted_ratio >= 0.99;
  if (cfg.rate_per_conn) {
    tick_ok = tick_ok && row.client_offered_ratio >= 0.99 &&
              row.client_pacing_lag_p99_us <= missed_budget;
  }
  row.client_tick_ok = tick_ok ? 1 : 0;
  return row;
}

}  // namespace rudp_bench
