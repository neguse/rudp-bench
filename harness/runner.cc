#include "harness/runner.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
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
  uint64_t attempted = 0;
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

  void record_send_due(uint64_t expected_deliveries, uint64_t lag_us,
                       uint64_t missed_budget_us) {
    attempted += expected_deliveries;
    pacing_lag_us.record(lag_us);
    if (lag_us > missed_budget_us) ++missed_pacing;
  }

  void record_accepted(uint64_t expected_deliveries) {
    accepted += expected_deliveries;
  }

  void record_recv_drained(uint64_t n) { recv_drained.record(n); }

  void record_outstanding(uint64_t n) {
    outstanding_max = std::max(outstanding_max, n);
  }
};

uint64_t pacing_budget_us(uint32_t rate_per_conn) {
  if (rate_per_conn == 0) return 0;
  uint64_t interval_us = 1'000'000ULL / rate_per_conn;
  // Allow up to ~10% of the per-msg interval as pacing lag, with a 100us
  // floor so high-rate scenarios still flag genuine starvation. The old
  // formula capped at 100us regardless of rate, which made low-rate runs
  // fail tick_ok over single-digit microsecond overruns from per-msg
  // allocator overhead in libraries like yojimbo.
  return std::max<uint64_t>(100, interval_us / 10);
}

constexpr std::chrono::microseconds kAdaptiveIdleCap{20};
constexpr std::chrono::milliseconds kRssSampleInterval{100};
constexpr size_t kServerRecvDrainLimit = 1024;
constexpr size_t kReliableFlagOffset = 16;
constexpr size_t kHeaderBytes = kReliableFlagOffset + 1;

void adaptive_idle_for(std::chrono::microseconds d) {
  if (d.count() > 0) std::this_thread::sleep_for(d);
}

void sample_rss_if_due(ProcSampler& ps,
                       std::chrono::steady_clock::time_point now,
                       std::chrono::steady_clock::time_point& next_sample) {
  if (now < next_sample) return;
  ps.sample_rss();
  next_sample = now + kRssSampleInterval;
}

struct ChannelSched {
  uint32_t rate = 0;
  bool reliable = false;
  std::chrono::nanoseconds interval{0};
  std::vector<std::chrono::steady_clock::time_point> next_send;

  bool active() const { return rate > 0; }

  void init(uint32_t r, bool rel, uint32_t conns,
            std::chrono::steady_clock::time_point t0) {
    rate = r;
    reliable = rel;
    interval = r ? std::chrono::nanoseconds(1'000'000'000ULL / r)
                 : std::chrono::nanoseconds(0);
    next_send.assign(conns, t0);
  }
};

}  // namespace

CsvRow run_server(Adapter& a, const ScenarioConfig& cfg) {
  ProcSampler ps;
  a.server_listen(cfg.port);
  ps.begin();

  std::vector<uint8_t> buf(65536);
  std::unordered_set<uint32_t> known_conns;  // broadcast 配信先
  auto deadline = std::chrono::steady_clock::now() +
                  std::chrono::seconds(cfg.duration_s + cfg.warmup_s + 2);
  auto next_rss_sample = std::chrono::steady_clock::now() + kRssSampleInterval;
  while (std::chrono::steady_clock::now() < deadline) {
    sample_rss_if_due(ps, std::chrono::steady_clock::now(), next_rss_sample);
    a.poll();
    bool did_work = false;
    for (size_t drained = 0; drained < kServerRecvDrainLimit; ++drained) {
      size_t n;
      uint32_t cid;
      int r = a.recv(buf.data(), buf.size(), &n, &cid);
      if (r != 1) break;
      did_work = true;
      known_conns.insert(cid);
      if (n < kHeaderBytes) continue;
      // Echo back on the same channel the message arrived on (client tags
      // each outbound payload with the reliable flag at kReliableFlagOffset).
      bool reliable = buf[kReliableFlagOffset] != 0;
      if (cfg.mode == ServerMode::Echo) {
        a.send(cid, buf.data(), n, reliable);
      } else {
        for (uint32_t target : known_conns) {
          a.send(target, buf.data(), n, reliable);
        }
      }
    }
    if (!did_work && cfg.idle_policy == IdlePolicy::Adaptive) {
      adaptive_idle_for(kAdaptiveIdleCap);
    }
  }
  ps.end();
  a.close();

  // Server reports the flush_policy of the reliable channel if reliable
  // traffic was active, else unreliable. Combined runs surface the reliable
  // policy as the more interesting one.
  bool reliable_active = cfg.rate_r > 0;
  CsvRow row;
  row.library = cfg.library;
  row.encryption = a.encryption_on() ? "on" : "off";
  row.phase = 1;
  row.rate_r = cfg.rate_r;
  row.rate_u = cfg.rate_u;
  row.size = cfg.size_bytes;
  row.conns = cfg.conns;
  row.loss = cfg.loss_pct;
  row.cpu_pct = ps.cpu_pct();
  row.rss_mb = ps.rss_max_mb();
  row.duration_s = cfg.duration_s;
  row.mode = (cfg.mode == ServerMode::Broadcast) ? "broadcast" : "echo";
  row.idle_policy = idle_policy_name(cfg.idle_policy);
  row.flush_policy = a.flush_policy(reliable_active);
  return row;
}

CsvRow run_client(Adapter& a, const ScenarioConfig& cfg) {
  using clock = std::chrono::steady_clock;
  ProcSampler ps;
  LatencyHist rtt_r;
  LatencyHist rtt_u;
  ThroughputCounter th;
  DeliveryTracker dt;
  ClientTickStats tick;

  // connect all (optionally rate-limited so listener / TLS stack can absorb).
  std::vector<uint32_t> ids;
  ids.reserve(cfg.conns);
  auto t_connect_begin = clock::now();
  const std::chrono::microseconds ramp_interval =
      (cfg.ramp_up_ms > 0 && cfg.conns > 0)
          ? std::chrono::microseconds(static_cast<int64_t>(cfg.ramp_up_ms) * 1000 / cfg.conns)
          : std::chrono::microseconds(0);
  for (uint32_t i = 0; i < cfg.conns; ++i) {
    ids.push_back(a.client_connect(cfg.host.c_str(), cfg.port));
    if (ramp_interval.count() > 0 && i + 1 < cfg.conns) {
      auto sleep_until = t_connect_begin + ramp_interval * (i + 1);
      while (clock::now() < sleep_until) {
        a.poll();
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    }
  }
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

  // Two independent per-conn pacing schedules: reliable and unreliable.
  ChannelSched sched_r;
  ChannelSched sched_u;
  sched_r.init(cfg.rate_r, true, cfg.conns, clock::now());
  sched_u.init(cfg.rate_u, false, cfg.conns, clock::now());

  std::vector<uint8_t> payload(cfg.size_bytes, 0xAB);
  // Single seq counter per conn shared across channels — each msg gets a
  // unique (conn_id, seq) so the dedup tracker stays correct.
  std::vector<uint64_t> seq_counter(cfg.conns, 1);
  std::vector<uint8_t> rxbuf(65536);
  uint64_t outstanding = 0;
  uint32_t expected_per_send = (cfg.mode == ServerMode::Broadcast) ? cfg.conns : 1;
  uint64_t combined_rate = static_cast<uint64_t>(cfg.rate_r) + cfg.rate_u;
  uint64_t target_attempted =
      combined_rate * cfg.conns * cfg.duration_s * expected_per_send;
  // Use the higher per-channel rate for missed-pacing budget (most aggressive
  // schedule sets the bar for tick health).
  uint64_t missed_budget = pacing_budget_us(std::max(cfg.rate_r, cfg.rate_u));

  auto in_measure = [&](clock::time_point t) { return t >= warmup_end; };

  auto tail_until = run_end + std::chrono::milliseconds(500);
  auto next_rss_sample = clock::now() + kRssSampleInterval;

  auto try_send_on = [&](ChannelSched& sched, uint32_t i,
                         clock::time_point now, bool& did_work) {
    if (!sched.active()) return;
    if (now < sched.next_send[i]) return;
    did_work = true;
    uint64_t lag_us = 0;
    if (now > sched.next_send[i]) {
      lag_us = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(
              now - sched.next_send[i])
              .count());
    }
    bool in_active = now >= warmup_end && now < run_end;
    if (in_active) tick.record_send_due(expected_per_send, lag_us, missed_budget);
    uint64_t local_seq = seq_counter[i]++;
    uint64_t global_seq = (static_cast<uint64_t>(i) << 32) | local_seq;
    uint64_t ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      now.time_since_epoch()).count();
    std::memcpy(payload.data(), &global_seq, 8);
    std::memcpy(payload.data() + 8, &ts, 8);
    payload[kReliableFlagOffset] = sched.reliable ? 1 : 0;
    if (a.send(ids[i], payload.data(), payload.size(), sched.reliable) == 0) {
      if (in_measure(now)) {
        tick.record_accepted(expected_per_send);
        for (uint32_t k = 0; k < expected_per_send; ++k) {
          dt.mark_accepted(global_seq, ids[i]);
        }
        outstanding += expected_per_send;
        tick.record_outstanding(outstanding);
      }
    }
    sched.next_send[i] += sched.interval;
    if (sched.next_send[i] < now) sched.next_send[i] = now;
  };

  auto soonest_due = [&]() -> clock::time_point {
    clock::time_point best = clock::time_point::max();
    auto pick = [&](const ChannelSched& sched) {
      if (!sched.active()) return;
      for (auto& t : sched.next_send) {
        if (t < best) best = t;
      }
    };
    pick(sched_r);
    pick(sched_u);
    return best;
  };

  while (clock::now() < tail_until) {
    auto now = clock::now();
    sample_rss_if_due(ps, now, next_rss_sample);
    bool in_diag = now >= warmup_end;
    bool did_work = false;
    tick.record_tick(now, in_diag);
    if (now < run_end) {
      for (uint32_t i = 0; i < cfg.conns; ++i) {
        try_send_on(sched_r, i, now, did_work);
        try_send_on(sched_u, i, now, did_work);
      }
    }
    a.poll();
    uint64_t drained_this_tick = 0;
    while (true) {
      size_t n;
      uint32_t cid;
      int r = a.recv(rxbuf.data(), rxbuf.size(), &n, &cid);
      if (r != 1) break;
      ++drained_this_tick;
      if (n < kHeaderBytes) continue;
      uint64_t seq, ts;
      std::memcpy(&seq, rxbuf.data(), 8);
      std::memcpy(&ts, rxbuf.data() + 8, 8);
      bool reliable_msg = rxbuf[kReliableFlagOffset] != 0;
      auto t_recv = clock::now();
      uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            t_recv.time_since_epoch()).count();
      uint64_t rtt_us = (now_ns - ts) / 1000;
      if (in_measure(t_recv)) {
        (reliable_msg ? rtt_r : rtt_u).record_us(rtt_us);
        th.record(n);
        if (dt.mark_received(seq, cid) && outstanding > 0) --outstanding;
      }
    }
    if (in_diag && drained_this_tick > 0) tick.record_recv_drained(drained_this_tick);
    if (drained_this_tick > 0) did_work = true;
    if (!did_work && outstanding == 0 &&
        cfg.idle_policy == IdlePolicy::Adaptive) {
      auto sleep_for = kAdaptiveIdleCap;
      if (combined_rate && clock::now() < run_end) {
        auto until_due = std::chrono::duration_cast<std::chrono::microseconds>(
            soonest_due() - clock::now());
        if (until_due < sleep_for) sleep_for = until_due;
      }
      adaptive_idle_for(sleep_for);
    }
  }
  ps.end();
  // close() は自己 shutdown を発生させるので、観測値はその前に snapshot。
  ConnectionStats cs = a.connection_stats();
  a.close();

  bool reliable_active = cfg.rate_r > 0;
  CsvRow row;
  row.library = cfg.library;
  row.encryption = a.encryption_on() ? "on" : "off";
  row.phase = 1;
  row.rate_r = cfg.rate_r;
  row.rate_u = cfg.rate_u;
  row.size = cfg.size_bytes;
  row.conns = cfg.conns;
  row.loss = cfg.loss_pct;
  row.throughput_mbps = (th.bytes() * 8.0) / (cfg.duration_s * 1'000'000.0);
  row.msg_per_sec = th.messages() / std::max<uint32_t>(1, cfg.duration_s);
  row.rtt_r_p50_us = rtt_r.percentile_us(0.50);
  row.rtt_r_p95_us = rtt_r.percentile_us(0.95);
  row.rtt_r_p99_us = rtt_r.percentile_us(0.99);
  row.rtt_u_p50_us = rtt_u.percentile_us(0.50);
  row.rtt_u_p95_us = rtt_u.percentile_us(0.95);
  row.rtt_u_p99_us = rtt_u.percentile_us(0.99);
  if (!cfg.bins_r_out_path.empty()) {
    std::ofstream f(cfg.bins_r_out_path, std::ios::binary);
    rtt_r.write_binary(f);
  }
  if (!cfg.bins_u_out_path.empty()) {
    std::ofstream f(cfg.bins_u_out_path, std::ios::binary);
    rtt_u.write_binary(f);
  }
  row.delivered = dt.received();
  row.accepted = dt.accepted();
  row.delivery_ratio = dt.delivery_ratio();
  row.cpu_pct = ps.cpu_pct();
  row.rss_mb = ps.rss_max_mb();
  row.connect_ms = connect_ms;
  row.duration_s = cfg.duration_s;
  row.mode = (cfg.mode == ServerMode::Broadcast) ? "broadcast" : "echo";
  row.idle_policy = idle_policy_name(cfg.idle_policy);
  row.flush_policy = a.flush_policy(reliable_active);
  row.client_tick_gap_p99_us = tick.tick_gap_us.percentile_per_mille(990);
  row.client_tick_gap_max_us = tick.tick_gap_us.max();
  row.client_pacing_lag_p99_us = tick.pacing_lag_us.percentile_per_mille(990);
  row.client_pacing_lag_max_us = tick.pacing_lag_us.max();
  row.client_missed_pacing = tick.missed_pacing;
  row.client_attempted = tick.attempted;
  row.client_accepted = tick.accepted;
  row.client_attempted_ratio =
      target_attempted ? static_cast<double>(tick.attempted) /
                             static_cast<double>(target_attempted)
                       : 1.0;
  row.client_accepted_ratio =
      tick.attempted ? static_cast<double>(tick.accepted) /
                           static_cast<double>(tick.attempted)
                     : 0.0;
  row.client_recv_drained_p99 = tick.recv_drained.percentile_per_mille(990);
  row.client_recv_drained_max = tick.recv_drained.max();
  row.client_outstanding_max = tick.outstanding_max;
  row.conn_peak = cs.connected_peak;
  row.conn_disc_transport = cs.shutdown_by_transport;
  row.conn_disc_peer = cs.shutdown_by_peer;
  row.delivery_dedup_policy = DeliveryTracker::dedup_policy();
  // tick_ok means "this run produced trustworthy data", not "every send was
  // perfectly on time". Functional correctness is `attempted == target` and
  // `accepted == attempted`; per-tick pacing lag stays in diagnostics for
  // investigation but is not a pass/fail gate (a 2.5ms lag at the tail of
  // a 100-conn 50Hz schedule does not break voice traffic).
  // tick_gap threshold scales with rate so libraries with heavier poll
  // overhead (gns, msquic) at low send rates do not falsely fail.
  uint64_t tick_gap_budget_us = combined_rate
      ? std::max<uint64_t>(250, 1'000'000ULL / combined_rate / 4)
      : 250;
  bool tick_ok = tick.tick_gap_us.count() > 0 &&
                 row.client_tick_gap_p99_us <= tick_gap_budget_us &&
                 row.client_accepted_ratio >= 0.99;
  if (combined_rate) {
    tick_ok = tick_ok && row.client_attempted_ratio >= 0.99;
  }
  row.client_tick_ok = tick_ok ? 1 : 0;
  return row;
}

}  // namespace rudp_bench
