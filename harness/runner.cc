#include "harness/runner.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
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
constexpr size_t kServerRecvDrainLimit = std::numeric_limits<size_t>::max();
constexpr size_t kReliableFlagOffset = 16;
constexpr size_t kHeaderBytes = kReliableFlagOffset + 1;
constexpr uint8_t kPayloadFlagReliable = 0x01;
constexpr uint8_t kPayloadFlagMeasured = 0x02;

void adaptive_idle_for(std::chrono::microseconds d) {
  if (d.count() > 0) std::this_thread::sleep_for(d);
}

void sample_proc_if_due(ProcSampler& ps,
                        std::chrono::steady_clock::time_point now,
                        std::chrono::steady_clock::time_point& next_sample) {
  if (now < next_sample) return;
  ps.sample_rss();
  ps.sample_cpu();  // M1: per-interval CPU so spikes surface as cpu_pct_peak
  next_sample = now + kRssSampleInterval;
}

size_t server_recv_drain_limit() {
  const char* v = std::getenv("RUDP_SERVER_RECV_DRAIN_LIMIT");
  if (!v || !*v) return kServerRecvDrainLimit;
  errno = 0;
  char* end = nullptr;
  unsigned long long parsed = std::strtoull(v, &end, 10);
  if (end == v || *end != '\0' || errno == ERANGE) {
    return kServerRecvDrainLimit;
  }
  if (parsed == 0) return std::numeric_limits<size_t>::max();
  if (parsed > std::numeric_limits<size_t>::max()) {
    return std::numeric_limits<size_t>::max();
  }
  return static_cast<size_t>(parsed);
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
  a.hint_connections(cfg.conns);
  a.server_listen(cfg.port);
  ps.begin();

  std::vector<uint8_t> buf(65536);
  std::unordered_set<uint32_t> known_conns;  // broadcast 配信先
  std::vector<uint32_t> known_conn_ids;
  std::vector<uint8_t> known_conn_seen(static_cast<size_t>(cfg.conns) + 1, 0);
  auto remember_conn = [&](uint32_t cid) {
    if (cid < known_conn_seen.size()) {
      if (known_conn_seen[cid]) return;
      known_conn_seen[cid] = 1;
      known_conn_ids.push_back(cid);
      return;
    }
    if (known_conns.insert(cid).second) {
      known_conn_ids.push_back(cid);
    }
  };
  auto start = std::chrono::steady_clock::now();
  auto server_tail_ms = std::max<uint32_t>(2000, cfg.tail_ms + 500);
  // The client spends ~ramp_up_ms connecting before its warmup+duration
  // window even starts, so the server lifetime must cover ramp too.
  // Without this the server exits mid-run and the tail of the client's send
  // window is "lost" — at ramp=10s/duration=20s that reads as delivery≈0.6,
  // an artifact that was misattributed to the library under test.
  auto deadline = start + std::chrono::seconds(cfg.duration_s + cfg.warmup_s) +
                  std::chrono::milliseconds(cfg.ramp_up_ms) +
                  std::chrono::milliseconds(server_tail_ms);
  // M2: exclude warmup from the server CPU window too. The server has no
  // explicit warmup gate (it just reacts), so re-baseline once warmup elapses.
  // Shift by ramp_up_ms as well: the client is still connecting during ramp,
  // so including it would dilute the server CPU average with idle time.
  auto measure_begin = start + std::chrono::seconds(cfg.warmup_s) +
                       std::chrono::milliseconds(cfg.ramp_up_ms);
  bool measure_started = (cfg.warmup_s == 0);
  auto next_rss_sample = start + kRssSampleInterval;
  uint64_t server_received = 0;
  uint64_t server_echo_accepted = 0;
  uint64_t server_received_r = 0;
  uint64_t server_received_u = 0;
  uint64_t server_echo_accepted_r = 0;
  uint64_t server_echo_accepted_u = 0;
  BoundedHistogram server_recv_drained{100'000};
  size_t recv_drain_limit = server_recv_drain_limit();
  uint32_t server_tick_ctr = 0;
  auto loop_now = std::chrono::steady_clock::now();
  for (;;) {
    if ((++server_tick_ctr & 255) == 0) {
      loop_now = std::chrono::steady_clock::now();
      if (loop_now >= deadline) break;
      if (!measure_started && loop_now >= measure_begin) {
        ps.mark_measure_begin();
        measure_started = true;
      }
      sample_proc_if_due(ps, loop_now, next_rss_sample);
    }
    a.poll();
    bool did_work = false;
    size_t drained_this_tick = 0;
    for (size_t drained = 0; drained < recv_drain_limit; ++drained) {
      size_t n;
      uint32_t cid;
      int r = a.recv(buf.data(), buf.size(), &n, &cid);
      if (r != 1) break;
      ++drained_this_tick;
      did_work = true;
      remember_conn(cid);
      if (n < kHeaderBytes) continue;
      // Echo back on the same channel the message arrived on (client tags
      // each outbound payload with the reliable flag at kReliableFlagOffset).
      bool reliable = (buf[kReliableFlagOffset] & kPayloadFlagReliable) != 0;
      bool measured = (buf[kReliableFlagOffset] & kPayloadFlagMeasured) != 0;
      if (measured) {
        ++server_received;
        if (reliable) {
          ++server_received_r;
        } else {
          ++server_received_u;
        }
      }
      if (cfg.mode == ServerMode::Echo) {
        if (a.send(cid, buf.data(), n, reliable) == 0 && measured) {
          ++server_echo_accepted;
          if (reliable) {
            ++server_echo_accepted_r;
          } else {
            ++server_echo_accepted_u;
          }
        }
      } else {
        size_t accepted = a.send_many(known_conn_ids.data(), known_conn_ids.size(),
                                      buf.data(), n, reliable);
        if (measured) {
          server_echo_accepted += accepted;
          if (reliable) {
            server_echo_accepted_r += accepted;
          } else {
            server_echo_accepted_u += accepted;
          }
        }
      }
    }
    if (drained_this_tick > 0) server_recv_drained.record(drained_this_tick);
    if (!did_work && cfg.idle_policy == IdlePolicy::Adaptive) {
      adaptive_idle_for(kAdaptiveIdleCap);
    }
  }
  ps.end();
  ConnectionStats cs = a.connection_stats();
  auto t_close0 = std::chrono::steady_clock::now();
  a.close();
  uint64_t close_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t_close0).count();

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
  row.server_received = server_received;
  row.server_echo_accepted = server_echo_accepted;
  row.server_received_r = server_received_r;
  row.server_received_u = server_received_u;
  row.server_echo_accepted_r = server_echo_accepted_r;
  row.server_echo_accepted_u = server_echo_accepted_u;
  row.server_recv_drained_p99 = server_recv_drained.percentile_per_mille(990);
  row.server_recv_drained_max = server_recv_drained.max();
  row.cpu_pct = ps.cpu_pct();
  row.cpu_pct_peak = ps.cpu_pct_peak();
  row.rss_mb = ps.rss_max_mb();
  row.close_ms = close_ms;
  row.duration_s = cfg.duration_s;
  row.mode = (cfg.mode == ServerMode::Broadcast) ? "broadcast" : "echo";
  row.idle_policy = idle_policy_name(cfg.idle_policy);
  row.flush_policy = a.flush_policy(reliable_active);
  row.conn_peak = cs.connected_peak;
  row.conn_disc_transport = cs.shutdown_by_transport;
  row.conn_disc_peer = cs.shutdown_by_peer;
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
  a.hint_connections(cfg.conns);
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
  // Always consume the FULL ramp window, even after the last connect. The
  // canonical harness splits one room across several client processes, each
  // ramping only its own conns share: without this tail wait a 1-conn process
  // skips the ramp entirely while a 2-conn sibling burns ramp/2, so the
  // processes' measurement windows end up offset by up to ramp_up_ms. In
  // broadcast mode that offset reads as a delivery deficit (senders missing
  // from the edges of each receiver's window) and the fanout to already-exited
  // receivers wedges in the transport's send queue. Aligning every process to
  // the same connect-phase length removes the artifact.
  if (cfg.ramp_up_ms > 0) {
    auto ramp_end = t_connect_begin + std::chrono::milliseconds(cfg.ramp_up_ms);
    while (clock::now() < ramp_end) {
      a.poll();
      std::this_thread::sleep_for(std::chrono::microseconds(100));
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
  uint64_t accepted_r = 0;
  uint64_t accepted_u = 0;
  uint64_t delivered_r = 0;
  uint64_t delivered_u = 0;
  uint32_t expected_per_send =
      (cfg.mode == ServerMode::Broadcast) ? cfg.fanout_conns : 1;
  uint64_t combined_rate = static_cast<uint64_t>(cfg.rate_r) + cfg.rate_u;
  uint64_t target_attempted =
      combined_rate * cfg.conns * cfg.duration_s * expected_per_send;
  // Use the higher per-channel rate for missed-pacing budget (most aggressive
  // schedule sets the bar for tick health).
  uint64_t missed_budget = pacing_budget_us(std::max(cfg.rate_r, cfg.rate_u));

  auto in_measure = [&](clock::time_point t) { return t >= warmup_end; };

  auto tail_until = run_end + std::chrono::milliseconds(cfg.tail_ms);
  auto next_rss_sample = clock::now() + kRssSampleInterval;
  bool measure_started = (cfg.warmup_s == 0);  // M2: re-baseline CPU at warmup_end

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
    uint64_t origin_id = static_cast<uint64_t>(cfg.conn_id_offset) + i;
    uint64_t global_seq = (origin_id << 32) | local_seq;
    uint64_t ts = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      now.time_since_epoch()).count();
    std::memcpy(payload.data(), &global_seq, 8);
    std::memcpy(payload.data() + 8, &ts, 8);
    payload[kReliableFlagOffset] =
        (sched.reliable ? kPayloadFlagReliable : 0) |
        (in_active ? kPayloadFlagMeasured : 0);
    if (a.send(ids[i], payload.data(), payload.size(), sched.reliable) == 0) {
      // M4: gate accepted on the SAME bounded window as attempted (in_active).
      // try_send_on only runs while now < run_end so this matches in_measure in
      // practice, but using in_active makes the symmetry explicit and removes a
      // latent off-by-a-tick drift in attempted_ratio if the caller ever sends
      // past run_end. The recv-side delivery marking keeps the unbounded
      // in_measure() because tail echoes legitimately arrive after run_end.
      if (in_active) {
        tick.record_accepted(expected_per_send);
        for (uint32_t k = 0; k < expected_per_send; ++k) {
          dt.mark_accepted(global_seq, ids[i]);
        }
        if (sched.reliable) {
          accepted_r += expected_per_send;
        } else {
          accepted_u += expected_per_send;
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

  for (auto now = clock::now(); now < tail_until; now = clock::now()) {
    if (!measure_started && now >= warmup_end) {
      ps.mark_measure_begin();  // M2: drop warmup from the CPU window
      measure_started = true;
    }
    sample_proc_if_due(ps, now, next_rss_sample);
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
      bool reliable_msg = (rxbuf[kReliableFlagOffset] & kPayloadFlagReliable) != 0;
      bool measured_msg = (rxbuf[kReliableFlagOffset] & kPayloadFlagMeasured) != 0;
      auto t_recv = clock::now();
      uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            t_recv.time_since_epoch()).count();
      uint64_t rtt_us = (now_ns - ts) / 1000;
      if (in_measure(t_recv) && measured_msg) {
        (reliable_msg ? rtt_r : rtt_u).record_us(rtt_us);
        th.record(n);
        if (dt.mark_received(seq, cid)) {
          if (reliable_msg) {
            ++delivered_r;
          } else {
            ++delivered_u;
          }
          if (outstanding > 0) --outstanding;
        }
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
  // L6: time teardown. A lib that cannot synchronously tear down (e.g. msquic,
  // whose close() is a deliberate no-op) shows close_ms≈0, which is itself the
  // lifecycle-health signal — it is not measuring a clean shutdown.
  auto t_close0 = clock::now();
  a.close();
  uint64_t close_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          clock::now() - t_close0).count();

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
  row.delivered_r = delivered_r;
  row.delivered_u = delivered_u;
  row.accepted_r = accepted_r;
  row.accepted_u = accepted_u;
  row.delivery_ratio = dt.delivery_ratio();
  row.cpu_pct = ps.cpu_pct();
  row.cpu_pct_peak = ps.cpu_pct_peak();
  row.rss_mb = ps.rss_max_mb();
  row.close_ms = close_ms;
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
  // perfectly on time". Functional correctness is `attempted == target`
  // (client applied the intended offered load) and `accepted == attempted`
  // (the adapter accepted it). Per-tick pacing lag (client_tick_gap_p99_us)
  // is a smoothness DIAGNOSTIC, not a pass/fail gate: a single-loop load
  // generator legitimately bursts at high conns (e.g. ~26ms tick gap at 1000
  // conns) while still emitting 100% of scheduled sends (attempted_ratio==1).
  // Gating on tick_gap there falsely invalidated otherwise-trustworthy
  // throughput/delivery measurements (notably gns/msquic at high conns). The
  // gap is recorded in diagnostics for latency-quality investigation instead.
  bool tick_ok = row.client_accepted_ratio >= 0.99;
  if (combined_rate) {
    tick_ok = tick_ok && row.client_attempted_ratio >= 0.99;
  }
  row.client_tick_ok = tick_ok ? 1 : 0;
  return row;
}

}  // namespace rudp_bench
