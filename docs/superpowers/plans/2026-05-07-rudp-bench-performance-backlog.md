# RUDP Bench Performance Review Backlog

> Source: 2026-05-07 performance review. 目的は、ライブラリ本体の性能差と harness / adapter 起因の測定汚染を分離し、Phase 1 / Phase 2 の結果を ranking に使える状態へ近づけること。

## Status Key

- [x] Done
- [ ] Todo

## Guiding Rules

- このベンチの主問いは「指定した負荷条件で、到達率を維持し、RTT tail を安定して低く保てるか」。
- 結果を見る側は `delivery_ratio`, `rtt_p50_us`, `rtt_p95_us`, `rtt_p99_us`, `server_cpu_pct` に絞る。
- `rtt_p50_us` は補助指標。主判断は `delivery_ratio` と `rtt_p95_us` / `rtt_p99_us`。
- client CPU は ranking objective にしない。client は負荷発生器 + RTT 計測器として扱う。
- client tick / accepted ratio / process status は diagnostic として残し、canonical result には詳細を出さない。
- `valid=false` の canonical row は ranking から除外する。詳細原因は diagnostics を見る。
- 既存 CSV 互換性は維持しない。曖昧な列名や混ざった責務は壊してよい。
- payload truncate は禁止。実接続数を満たせない `conns` も禁止。どちらも unsupported として扱う。

## Target Output Model

成果物は2層に分ける。

### Canonical Result

テスト結果として信用してよい最終成果物。比較、表、グラフ、ranking はこれだけを見る。

粒度:
- 1 scenario = 1 row

必須列:

```text
run_id
scenario_id
library
valid
invalid_reason
delivery_ratio
rtt_p50_us
rtt_p95_us
rtt_p99_us
server_cpu_pct
```

必要なら後で足す候補:
- `connect_ms`: 接続コストを見る別用途で必要な場合
- `server_rss_mb`: メモリ比較を主目的にする場合

入れないもの:
- client CPU
- client tick の詳細値
- attempted / accepted の詳細
- raw server/client row
- stderr path

### Diagnostic Summary

テストがうまく行えたか調査するためのログ。常時出してよいが、ranking には直接使わない。

粒度:
- 1 scenario x role = 1 row

代表列:

```text
run_id
scenario_id
role
exit_reason
exit_status
cpu_pct
rss_mb
attempted
accepted
delivered
accepted_ratio
delivery_ratio
client_tick_ok
client_tick_gap_p99_us
client_pacing_lag_p99_us
raw_result_path
stdout_path
stderr_path
delivery_dedup_policy
```

`client_tick_ok=false` などの diagnostic failure は reducer が canonical の `valid=false` / `invalid_reason=client_tick` に畳む。

## Completed Baseline Work

- [x] **PERF-000: Add client tick diagnostics**

  Files:
  - `harness/runner.cc`
  - `harness/csv_writer.*`
  - `harness/metrics.*`
  - `adapters/litenetlib/Program.cs`
  - `scripts/run_phase1*.sh`
  - `tests/test_*`
  - `README.md`

  Done:
  - Added `client_tick_gap_*`, `client_pacing_lag_*`, `client_attempted`, `client_accepted`, `client_*_ratio`, `client_recv_drained_*`, `client_outstanding_max`, `client_tick_ok`.
  - Preserved existing CSV column order and appended diagnostic columns as an interim step before the canonical/diagnostic split.
  - Verified with `ctest --test-dir build --output-on-failure`.

## P0: Measurement Validity

- [x] **PERF-001: Replace current CSV with canonical result + diagnostic summary**

  Problem:
  - Current output mixes scenario conditions, client metrics, server metrics, diagnostic counters, and ranking metrics into one role-local CSV. The result viewer sees too much noise, while server CPU can still be lost by scripts that append only client rows.

  Tasks:
  - Define canonical `results.csv` with only:
    - `run_id`
    - `scenario_id`
    - `library`
    - `valid`
    - `invalid_reason`
    - `delivery_ratio`
    - `rtt_p50_us`
    - `rtt_p95_us`
    - `rtt_p99_us`
    - `server_cpu_pct`
  - Define `diagnostics.csv` for role-level process and harness diagnostics.
  - Make runner scripts reduce server/client raw outputs into one canonical row per scenario.
  - Move `client_*`, attempted/accepted, exit status, raw paths, and role CPU/RSS into diagnostics.
  - Remove or rewrite old CSV writer semantics. Compatibility with old `sent` / role-local rows is not required.

  Acceptance:
  - `results.csv` has one row per scenario and contains no client tick detail columns.
  - `diagnostics.csv` contains one row per scenario x role and has enough detail to explain `valid=false`.
  - Plot/reporting tools read canonical results by default.
  - `ctest --test-dir build --output-on-failure` passes.

  Done:
  - Added `scripts/reduce_result.py`.
  - Updated phase runners to produce canonical `results.csv`, `diagnostics.csv`, and `scenarios.csv`.
  - Raw role CSVs and per-role stdout/stderr logs are kept under `*_raw/<run_id>/` and linked from diagnostics.
  - Reduced server grace from +5s to +2s so server rows normally complete before reducer merge.

- [x] **PERF-002: Add reducer validation rules for canonical `valid`**

  Problem:
  - The final result needs to distinguish "bad library result" from "test did not run correctly". Low delivery can be a valid result; client tick failure is not.

  Tasks:
  - Implement validation priority:
    - `unsupported_reliability`
    - `unsupported_payload`
    - `unsupported_conns`
    - `missing_binary`
    - `server_timeout`
    - `client_timeout`
    - `server_crash`
    - `client_crash`
    - `client_tick`
    - `no_accepted_messages`
    - `ok`
  - Treat low `delivery_ratio` as a valid performance result unless caused by execution failure.
  - Treat low `accepted_ratio` as valid overload/backpressure evidence unless client tick or process status says the test failed.
  - Fold detailed causes into canonical `invalid_reason`.
  - Keep detailed evidence in diagnostics.

  Acceptance:
  - Every canonical row has `valid` and `invalid_reason`.
  - Invalid rows are excluded by default from ranking tables.
  - Diagnostics explain every non-`ok` invalid reason.

  Done:
  - Reducer now applies ordered invalid reasons for unsupported reliability/payload/conns, missing adapter binary, timeout, crash, client tick failure, and zero accepted messages.
  - Phase runners pass server/client process exit status into diagnostics.
  - Low `delivery_ratio` remains a valid performance result.
  - Added reducer tests for each invalid category.

- [x] **PERF-003: Track attempted, accepted, and delivered in diagnostics**

  Problem:
  - Runner counts `sent` only when `Adapter::send()` succeeds. Backpressure, queue-full, and `CanSendMessage=false` need to be visible for investigation, but not shown in the normal result view.

  Tasks:
  - Track `attempted`, `accepted`, `delivered`, and `accepted_ratio` in diagnostics.
  - Define `delivery_ratio = delivered / accepted` for canonical result.
  - Reflect same logic in LiteNetLib, where `Send()` does not expose success.
  - Remove ambiguous `sent` from the new output model.

  Acceptance:
  - Overload can be diagnosed from `diagnostics.csv`.
  - Canonical result remains focused on delivery and RTT.

  Done:
  - Renamed raw role CSV `sent` to `accepted` and `client_offered` to `client_attempted`.
  - C++ and LiteNetLib runners now count attempted/accepted as expected delivery slots, so broadcast uses the same denominator as delivery.
  - Reducer fills diagnostics `attempted`, `accepted`, `delivered`, and recomputes `delivery_ratio = delivered / accepted`.
  - Legacy raw `sent` / `client_offered` remain reducer fallbacks only.

- [x] **PERF-004: Decide and encode idle policy instead of ad hoc sleeps**

  Problem:
  - Client currently spins, server sleeps 50us when idle. This creates asymmetric CPU and latency bias.

  Tasks:
  - Add `--idle=spin|adaptive` or equivalent scenario option.
  - Record `idle_policy` in scenario metadata or diagnostics.
  - For latency-focused runs, use spin or no sleep while outstanding echos exist.
  - For throughput-focused runs, allow adaptive sleep only when no send/recv work is due.
  - Validate adaptive policy against raw_udp spin baseline using RTT p99 delta.

  Acceptance:
  - Every scenario has an explicit idle policy in metadata or diagnostics.
  - Adaptive mode does not exceed the accepted raw_udp RTT p99 delta threshold.
  - `client_tick_ok` remains the diagnostic gate, not client CPU.

  Done:
  - Added `--idle=spin|adaptive` to C++ harness and LiteNetLib.
  - Default policy is `spin` for latency-focused runs.
  - `adaptive` only idles when no send/recv work is due and the client has no outstanding expected delivery.
  - Phase runners pass idle policy through to server/client and record it in `scenarios.csv`.
  - Raw role CSVs include `idle_policy` for standalone runs.

- [x] **PERF-005: Make Phase 1 script match the intended benchmark matrix**

  Problem:
  - Current sweep uses `size=64,1000`, `conns=1,50`, `rate=50`, `loss=0`. Design calls for larger payloads, higher conns, loss injection, and saturation-rate exploration.

  Tasks:
  - Add configurable scenario matrix for size, conns, rate, loss, mode, reliability.
  - Restore or explicitly revise the Phase 1 matrix in docs.
  - Add a saturation-rate discovery helper: 100 -> 1k -> 10k -> 100k msg/sec until delivery/accepted ratio drops or CPU saturates.
  - Ensure unsupported combinations become canonical `valid=false` rows with explicit `invalid_reason`.

  Acceptance:
  - Running Phase 1 can reproduce the documented matrix.
  - Quick sweep remains available as smoke, clearly named as such.

  Done:
  - `scripts/run_phase1.sh` now accepts configurable `--reliabilities`, `--sizes`, `--conns`, `--rates`, `--losses`, `--modes`, `--duration`, and `--warmup` axes.
  - Conservative default matrix is explicit, and the historical stress matrix is documented as a reproducible command.
  - Added `scripts/run_saturation.py` for 100 -> 1k -> 10k -> 100k msg/sec/conn discovery.
  - Saturation helper defaults to `adaptive` idle and stops on canonical `delivery_ratio`, diagnostics `accepted_ratio`, or canonical `server_cpu_pct`; client CPU remains diagnostic-only.
  - Quick sweep remains a fixed-axis smoke runner.

- [x] **PERF-006: Remove invalid large-payload scenarios or implement fragmentation**

  Problem:
  - 64KB payloads are invalid for raw UDP and currently get truncated or rejected by multiple adapters. yojimbo truncates to 4096B, mini_rudp receives into 2048B buffers, and raw UDP exceeds datagram limits.

  Tasks:
  - Choose one policy:
    - Lower max payload to a safe datagram size such as 8KB.
    - Or implement application-layer fragmentation where intended.
  - Add per-adapter `max_payload` or capability metadata.
  - Prevent silent truncation. Return failure or `na` for unsupported payload sizes.
  - Add tests for oversize payload behavior.

  Acceptance:
  - No adapter silently reports successful delivery with a smaller payload than requested.
  - Phase matrix does not include invalid payload sizes unless fragmentation is implemented.

  Done:
  - Chose the conservative policy: common Phase 1 payloads stay at `64,1000`; larger payloads must respect per-adapter caps.
  - Added `Adapter::max_payload_bytes(reliable)` and C++ entrypoint validation that emits a row without sending unsupported payloads.
  - Added LiteNetLib payload validation with a conservative 1000B cap.
  - Replaced yojimbo truncation with send failure for payloads above 4096B.
  - Increased mini_rudp receive buffers so payloads above 2042B are no longer silently truncated.
  - Updated reducer payload validation to use reliability-specific per-adapter caps.
  - Added oversize / larger-payload coverage in reducer and adapter tests.

## P1: Fairness Across Adapters

- [x] **PERF-007: Fix `conns` semantics for yojimbo**

  Problem:
  - `yojimbo` currently returns conn_id 0 and recreates the client object on each `client_connect()`, so `conns > 1` is not represented as multiple client connections.

  Tasks:
  - Decide whether yojimbo supports multi-conn in this harness.
  - If yes, create one client instance per conn or a supported multi-client structure.
  - If no, expose a max-conns capability and emit `na` for unsupported conns.
  - Add smoke coverage for `conns=2`.

  Acceptance:
  - `conns` reflects actual yojimbo connection count, or canonical output marks the scenario `unsupported_conns`.

  Done:
  - yojimbo client side now owns one `yojimbo::Client` instance per harness conn id instead of replacing a single client.
  - client-side receive paths preserve the local conn id, so echoed messages can be attributed to the correct requested connection.
  - Reducer now allows yojimbo `conns <= 64` and keeps `conns > 64` as `unsupported_conns`.
  - Added a two-client yojimbo smoke test and reducer coverage for `conns=2` and `conns=65`.

- [x] **PERF-008: Fix `conns` semantics for SLikeNet**

  Problem:
  - Multiple `client_connect()` calls to the same endpoint are mapped to one GUID / physical connection.

  Tasks:
  - Use independent client peer instances or another SLikeNet-supported model for multiple source endpoints.
  - Or expose max-conns / same-endpoint limitation and mark unsupported combinations.
  - Add a `conns=2` test proving server observes two connections.

  Acceptance:
  - `conns` axis measures real SLikeNet connection count.

  Done:
  - Chose the explicit unsupported policy for current SLikeNet adapter behavior.
  - Added `Adapter::max_connections()` and entrypoint validation so unsupported `conns` values emit skipped rows without opening sockets.
  - SLikeNet reports `max_connections() == 1`; reducer keeps `conns > 1` as `unsupported_conns`.
  - Added capability coverage for the SLikeNet connection cap.

- [x] **PERF-009: Rework msquic reliable mode to avoid one stream per message**

  Problem:
  - Reliable msquic sends open/start/close a new QUIC stream per message, adding heavy per-message overhead that is not representative for throughput/RTT comparison.

  Tasks:
  - Maintain a persistent stream per connection for reliable mode.
  - Keep length-prefix framing on the persistent stream.
  - Record stream policy in scenario metadata or diagnostics if both modes are kept.
  - Add tests for multiple reliable messages over one connection.

  Acceptance:
  - Reliable msquic no longer allocates and opens a stream for every message in the default benchmark path.

  Done:
  - msquic reliable mode now opens one outbound unidirectional stream per connection and reuses it.
  - Existing length-prefix framing is kept on the persistent stream.
  - The per-message stream policy was removed rather than kept as an alternate mode.
  - Added smoke coverage for two reliable messages over one connection.

- [x] **PERF-010: Normalize or expose flush/batching policy**

  Problem:
  - ENet flushes at `poll()` tail while raw_udp / mini_rudp send immediately. Other adapters have their own batching behavior.

  Tasks:
  - Add `flush_policy` metadata to diagnostics or scenario metadata.
  - Decide whether to force immediate flush where possible or leave library-native behavior.
  - Document comparison bias for latency vs throughput.

  Acceptance:
  - Diagnostic/scenario metadata makes batching policy visible, and docs state how to interpret it.

  Done:
  - Chose expose-over-normalize: adapter-native flushing remains unchanged.
  - Added `Adapter::flush_policy(reliable)` and raw role CSV `flush_policy`.
  - Added `flush_policy` to `scenarios.csv` and saturation summary output.
  - Documented how to interpret immediate, poll-driven, and library-internal policies.

## P1: Harness Overhead

- [x] **PERF-011: Bound latency sampling memory**

  Problem:
  - `LatencyHist::samples_` stores every RTT sample and sorts at the end. High throughput and long runs can make harness memory and final sort cost dominate.

  Tasks:
  - Replace full sample storage with reservoir sampling, fixed histogram, t-digest, or HDR-style histogram.
  - Keep p50/p95/p99 output compatible.
  - Add tests comparing percentile behavior on known distributions.

  Acceptance:
  - Latency memory use is bounded by configuration, not message count.

  Done:
  - Replaced full RTT sample vectors with bounded fixed histograms in C++ and LiteNetLib.
  - Kept p50/p95/p99 output columns unchanged.
  - Exact 1us bins are used through 10ms, 100us bins through 1s, and 1ms bins through 60s; larger values are tracked as overflow and return the observed max when selected by percentile.
  - Added tests for exact percentiles, bounded storage, and coarse-bin percentile behavior.

- [x] **PERF-012: Bound delivery dedup memory**

  Problem:
  - `DeliveryTracker::received_keys_` grows with every unique received message.

  Tasks:
  - Decide whether exact dedup is required for all runs.
  - If exact: bound by sliding window per connection.
  - If approximate: use a probabilistic structure or make dedup optional.
  - Add a metric showing dedup policy.

  Acceptance:
  - Delivery tracking memory no longer grows unbounded for long high-throughput runs.

  Done:
  - Replaced permanent exact dedup sets with exact sliding windows per received connection in C++ and LiteNetLib.
  - The dedup window is 65,536 sequence keys per connection, so memory is bounded by connection count rather than message count.
  - Added `delivery_dedup_policy` to raw role CSV and diagnostics.
  - Added tests for duplicate suppression, per-connection dedup, and window eviction.

- [x] **PERF-013: Sample RSS during the run**

  Problem:
  - `ProcSampler` checks RSS only at begin/end, so transient queue growth is missed.

  Tasks:
  - Add periodic RSS sampling in runner loop or a lightweight sampler thread.
  - Record `rss_max_mb` as true sampled max.
  - Keep CPU accounting based on process usage.

  Acceptance:
  - A test or stress run can show RSS max exceeding final RSS when memory is freed before end.

  Done:
  - `ProcSampler` now exposes `sample_rss()`; begin/end still bound CPU accounting.
  - C++ and LiteNetLib runner loops sample RSS about every 100ms and keep `rss_mb` as sampled max RSS.
  - Added coverage that samples a transient anonymous mapping before it is unmapped.

- [x] **PERF-014: Reduce adapter receive-path copies and allocations**

  Problem:
  - Many adapters copy incoming messages into `std::vector` / `byte[]` inboxes, then runner copies again into its buffer.

  Tasks:
  - Introduce reusable message buffers or a zero/one-copy receive API.
  - Reserve inbox storage where copy is unavoidable.
  - Apply changes per adapter with smoke tests.
  - Track whether any adapter-specific buffering changes semantics.

  Acceptance:
  - Hot receive path avoids per-message heap allocation where practical.

  Done:
  - Added `ReusableInboundQueue` for C++ adapters that need an inbox while keeping the existing copy-out `recv()` API.
  - Applied it to ENet, KCP, SLikeNet, UDT4, yojimbo, GNS, and msquic queued receive paths.
  - LiteNetLib now rents inbox byte arrays from `ArrayPool<byte>` instead of allocating with `GetRemainingBytes()`.
  - Added unit coverage for order/conn-id preservation, oversize drop, and buffer reuse.

## P2: Scalability Hotspots

- [x] **PERF-015: Replace raw_udp client O(conns) recv scan**

  Problem:
  - raw_udp client `recv()` scans all connection fds every call, which becomes O(conns) per receive attempt.

  Tasks:
  - Use `poll`, `epoll`, or shared-socket multiplexing.
  - Preserve per-connection identity.
  - Add a `conns=100+` smoke or benchmark sanity check.

  Acceptance:
  - raw_udp receive overhead scales with ready sockets rather than total conns.

  Done:
  - raw_udp client `poll()` now checks all client sockets once per tick and drains only sockets reported readable into `ReusableInboundQueue`.
  - raw_udp client `recv()` now pops queued packets, preserving per-client connection ids without an O(conns) scan per receive attempt.
  - Added a 128-connection smoke test that sends and echoes one datagram per client connection and verifies client-side conn-id identity.

- [x] **PERF-016: Replace mini_rudp client O(conns) recv scan**

  Problem:
  - mini_rudp mirrors the raw_udp scanning pattern and has the same high-conns bias.

  Tasks:
  - Apply the same readiness/multiplexing strategy as raw_udp.
  - Ensure reliable ACK/retransmit state remains per conn.

  Acceptance:
  - mini_rudp receive overhead scales with ready sockets rather than total conns.

  Done:
  - mini_rudp client `poll()` now readiness-checks client sockets and drains readable packets into `ReusableInboundQueue`.
  - ACK handling, reliable duplicate suppression, and retransmit state remain per connection before payloads are queued.
  - mini_rudp client `recv()` now pops queued payloads instead of scanning every connection fd.
  - Added a 128-connection reliable echo smoke test that verifies client-side conn-id identity.

- [x] **PERF-017: Optimize GNS polling**

  Problem:
  - GNS creates a connection vector every `poll()` and drains at most 64 messages per connection per poll.

  Tasks:
  - Reuse scratch connection storage or iterate under a carefully scoped lock.
  - Drain until empty or expose a configurable drain limit.
  - Add diagnostics for GNS backlog if available.

  Acceptance:
  - GNS adapter no longer allocates a connection vector on every tick in the hot path.

  Done:
  - Reused a member scratch connection vector in `poll()` so GNS no longer allocates a fresh connection list every tick.
  - Drained `ReceiveMessagesOnConnection` until the connection queue is empty instead of stopping at one 64-message batch.
  - Added a GNS smoke test that queues 96 reliable messages and verifies one poll can drain more than one receive batch.

- [x] **PERF-021: Bound mini_rudp reliable duplicate memory**

  Problem:
  - Harness-level delivery dedup is bounded, but `mini_rudp` kept every reliable sequence number ever received per connection. Long reliable runs could make adapter memory grow with message count and bias RSS / cache behavior.

  Tasks:
  - Reuse the same sliding-window duplicate suppression policy for adapter-internal reliable receive dedup.
  - Keep duplicate suppression bounded by connection count rather than message count.
  - Cover the sliding-window behavior with a unit test.

  Acceptance:
  - `mini_rudp` reliable duplicate suppression no longer stores an unbounded sequence set.

  Done:
  - Added shared `SlidingDedupWindow` and reused it from both `DeliveryTracker` and `mini_rudp`.
  - Kept the 65,536-key per-connection window policy.
  - Added direct unit coverage for window eviction and recent duplicate suppression.

- [x] **PERF-022: Reduce adapter send-path scratch allocations**

  Problem:
  - Some adapter send paths created a fresh temporary vector for every outbound message. At high msg/sec this adds allocator noise to latency and throughput comparisons.

  Tasks:
  - Replace per-send temporary buffers with adapter-owned scratch buffers where the underlying send call consumes/copies data synchronously.
  - Keep persistent storage where the protocol needs it, such as mini_rudp reliable retransmit queues.

  Acceptance:
  - Hot unreliable send paths avoid avoidable heap allocation per message.

  Done:
  - Reused scratch send buffers for mini_rudp unreliable packets, KCP unreliable bypass packets, and SLikeNet's one-byte message-id prefix buffer.
  - Left mini_rudp reliable packets owned by the retransmit queue.

- [x] **PERF-023: Remove shared KCP receive scratch buffer**

  Problem:
  - KCP `drain_socket()` used a function-static receive buffer. Server and client adapters can be polled concurrently in tests and benchmark roles, so the static buffer created avoidable shared mutable state.

  Tasks:
  - Move KCP receive scratch storage to the adapter instance.
  - Keep socket drain behavior unchanged.

  Acceptance:
  - KCP server/client adapter instances no longer share a receive scratch buffer.

  Done:
  - Replaced the static KCP receive buffer with an instance-owned fixed-size array.

## P2: Result Interpretation And Tooling

- [x] **PERF-018: Make reports use canonical results and hide diagnostics by default**

  Problem:
  - Reports should answer the main question directly: delivery ratio and RTT p50/p95/p99 under each scenario. Diagnostic details should not be shown unless investigating invalid rows.

  Tasks:
  - Build phase tables from canonical `results.csv`.
  - Default tables show only `delivery_ratio`, `rtt_p50_us`, `rtt_p95_us`, `rtt_p99_us`, and `server_cpu_pct`.
  - Hide diagnostics from normal reports.
  - Print a compact invalid-row summary by `invalid_reason`.

  Acceptance:
  - Normal reports can be read without understanding client tick or accepted counters.
  - Invalid rows are visible as invalid, with details delegated to diagnostics.

  Done:
  - `scripts/plot.py phase1-table` now reads canonical results and reports only delivery, RTT p50/p95/p99, and server CPU.
  - Invalid rows are excluded from metric pivots and summarized by `invalid_reason`.

- [x] **PERF-019: Add capability metadata for unsupported scenario axes**

  Problem:
  - Unsupported reliability is handled, but max payload, max conns, stream/datagram support, and batching policy are not consistently surfaced.

  Tasks:
  - Extend adapter capability reporting without forcing heavy runtime checks.
  - Emit `na` rows for unsupported payload/conns where appropriate.
  - Keep README capability table updated.

  Acceptance:
  - Invalid comparisons become canonical `valid=false` rows with explicit `invalid_reason`, not silent low throughput.

  Done:
  - Added a shared static capability table for reducer/reporting metadata: reliability support, payload cap, connection cap, transport mode, and flush policy.
  - Extended `scenarios.csv` with row-level capability metadata so unsupported axes can be explained without inspecting raw role CSVs.
  - Added ENet's 4095 peer cap to adapter metadata and reducer validation.
  - Updated README with the capability table and added reducer coverage for unsupported payload/conns metadata.

- [x] **PERF-020: Pin process roles or document scheduler policy**

  Problem:
  - On one host, spinning client can steal CPU from server/protocol workers if processes are not pinned.

  Tasks:
  - Add optional `taskset` / CPU pin flags to runner scripts.
  - Record pinning policy in result metadata.
  - Document recommended run environment.

  Acceptance:
  - Benchmark output records whether client/server were pinned.

  Done:
  - Added `--server-cpu` and `--client-cpu` to phase runners; specified roles are launched through `taskset -c`.
  - Added `server_cpu_pin`, `client_cpu_pin`, and `pinning_policy` to `scenarios.csv` and saturation summaries.
  - Documented the recommended same-host policy: pin server/client to separate CPUs for latency-focused runs, otherwise treat `pinning_policy=none` as OS-scheduled.

## Suggested Execution Order

1. PERF-001, PERF-002, PERF-018: establish canonical result and reports.
2. PERF-003: keep overload/backpressure evidence in diagnostics.
3. PERF-004, PERF-020: lock down scheduler/idle policy.
4. PERF-006, PERF-019: remove invalid scenario rows.
5. PERF-007, PERF-008, PERF-009: fix biggest adapter fairness issues.
6. PERF-011, PERF-012, PERF-013, PERF-014: reduce harness overhead.
7. PERF-015, PERF-016, PERF-017: high-connection scalability cleanup.
8. PERF-005, PERF-010: complete Phase 1/2 matrix and batching policy.
