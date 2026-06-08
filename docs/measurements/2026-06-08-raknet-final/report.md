# 最新 / Current RakNet Full Final Remeasure

**これが最新版 / canonical report:** RakNet 追加後の full-target final saturation remeasure。

**測定日時:** 2026-06-08 15:31-16:35 UTC (2026-06-09 00:31-01:35 JST)

**位置づけ:** RakNet を benchmark target に追加し、canonical final saturation profiles を current target set で N=3 再測定した。

## Change

- `raknet` adapter を追加した。bundled SLikeNet/RakNetLibStatic (RakNet 4.082 derived) を使うが、既存 `slikenet` adapter と異なり client 側は論理 connection ごとに `RakPeerInterface` を持つため、同一 server への多接続 benchmark に乗る。
- default benchmark target に `raknet` を追加した。
- current final target set に合わせ、`coop_rudp` も default list に含めた。

## Setup

- runner: `scripts/run_final_saturation_profiles.py`
- libraries: `coop_rudp,apex_rudp,litenetlib,enet,gns,raknet`
- netem: `25ms +/-5ms, 1% loss, limit=100000`
- pinning: server CPU `7,15`, client CPU `5,6,13,14`
- duration: 20s, tail: 500ms, idle: `adaptive`
- runs: N=3, aggregate valid gate: `min_valid=2`, break gate: median delivery `>= 0.95`

## Capacity

| profile | strongest | max OK | break | RakNet readout |
|---|---|---:|---:|---|
| `media_relay` | `apex_rudp` | 125 | 150 | break at 50, delivery 0.8251 |
| `game_server` | `apex_rudp` | 128 | 192 | OK at 64, break at 96 by client_tick |
| `echo` | `apex_rudp` | 3000 | not broken | break at 200 by valid_runs=1/3 |

## Per-Library Breakpoints

| profile | library | max OK | break | reason |
|---|---|---:|---:|---|
| `media_relay` | `apex_rudp` | 125 | 150 | delivery<0.95 |
| `media_relay` | `coop_rudp` | 100 | 125 | delivery<0.95 |
| `media_relay` | `litenetlib` | 50 | 75 | valid_runs=1/3 |
| `media_relay` | `raknet` | none | 50 | delivery<0.95 |
| `game_server` | `apex_rudp` | 128 | 192 | delivery<0.95 |
| `game_server` | `litenetlib` | 96 | 128 | client_tick |
| `game_server` | `gns` | 64 | 96 | valid_runs=1/3 |
| `game_server` | `raknet` | 64 | 96 | client_tick |
| `echo` | `apex_rudp` | 3000 | not broken | schedule upper bound |
| `echo` | `litenetlib` | 2000 | 3000 | client_tick |
| `echo` | `coop_rudp` | 1500 | 2000 | client_tick |
| `echo` | `enet` | 600 | 1000 | delivery<0.95 |
| `echo` | `gns` | 600 | 1000 | delivery<0.95 |
| `echo` | `raknet` | none | 200 | valid_runs=1/3 |

## RakNet Detail

| profile | conns | aggregate valid | valid runs | delivery | server CPU | note |
|---|---:|---:|---:|---:|---:|---|
| `media_relay` | 50 | 1 | 2/3 | 0.8251 | 121.81% | valid aggregate, but below delivery gate |
| `game_server` | 64 | 1 | 2/3 | 0.9805 | 58.25% | passes initial point |
| `game_server` | 96 | 0 | 0/3 | n/a | n/a | client_tick |
| `echo` | 200 | 0 | 1/3 | 0.9841 | 72.61% | aggregate invalid |

## Readout

RakNet can sustain the 64-player game profile, but does not pass the lowest media relay or echo points under this benchmark's validity gates. The failure mode is not unsupported capability: RakNet supports reliable and unreliable messages and the scenario metadata records `max_connections=4096`. The limiting factor in these profiles is pacing/return delivery under broadcast or mixed echo load.

This run changes the final comparison baseline: with the current worktree, `apex_rudp` is the strongest target on all three final profiles.

## Data

- profile definitions: [`data/profiles.csv`](data/profiles.csv)
- capacity: [`data/capacity.csv`](data/capacity.csv)
- medians: [`data/summary.csv`](data/summary.csv)
- run-level results: [`data/results_all.csv`](data/results_all.csv)
- scenarios: [`data/scenarios_all.csv`](data/scenarios_all.csv)
- raw run: `results/final_saturation_profiles_full_raknet_20260608T153112Z/` (`results/` is gitignored)
