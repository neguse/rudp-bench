# Production profiles fixed-conn benchmark

**測定日:** 2026-06-02

**位置づけ:** 固定 conn の production profile 測定。最終アウトプットは connection count を壊れるまで上げる [`../2026-06-02-final-saturation/report.md`](../2026-06-02-final-saturation/report.md) に移行した。

## Profiles

| profile | use case | mode | rate_r | rate_u | size | conns | 意図 |
|---|---|---:|---:|---:|---:|---:|---|
| `media_relay` | media SFU / relay | broadcast | 0 | 30 | 1000 | 50 | 50 publisher が 30Hz の near-MTU media packet を full-room fanout |
| `game_server` | authoritative game server | broadcast | 1 | 20 | 128 | 64 | 64-player arena。20Hz unreliable state/input fanout + 1Hz reliable gameplay event |

`broadcast` は harness の 1:N mode で、server は受信 payload を既知 connection 全体へ fanout する。broadcast の canonical denominator を正しく保つため、この測定は `client-procs=1` で実行した。

## Setup

- ホスト: Ryzen 7 PRO 5750GE。`scripts/bench_isolate.sh setup`。
- server CPU: `7,15`、client CPU: `5,6,13,14`。
- netem: `sudo scripts/netem.sh apply 25 5 1 100000`。
- duration: 20s、warmup: C++ adapters 2s / LiteNetLib 5s、tail: 500ms。
- idle: `adaptive`。
- libraries: `apex_rudp,litenetlib,enet,gns`。
- N=3、`scripts/aggregate_runs.py --min-valid 2` で valid run の中央値を採用。
- runner: `scripts/run_production_profiles.sh`。
- LiteNetLib は broadcast hot path 修正後に同一 profile で N=3 再測定し、`data/*` の LiteNetLib 行だけ差し替えた。

## Media Relay

| library | valid | delivery | forward_u | return_u | server CPU | rtt_u p99 |
|---|---:|---:|---:|---:|---:|---:|
| `litenetlib` | 3/3 | 0.9803 | 0.9902 | 0.9900 | 92.35% | 97.8ms |
| `apex_rudp` | 3/3 | 0.9792 | 0.9858 | 0.9900 | 83.03% | 75.2ms |
| `enet` | 3/3 | 0.9486 | 0.9876 | 0.9605 | 76.22% | 76.7ms |
| `gns` | 3/3 | 0.1530 | 0.9903 | 0.9036 | 84.72% | 2136.0ms |

Media relay は LiteNetLib / `apex_rudp` が delivery でほぼ横並び。LiteNetLib は broadcast hot path 修正で delivery が 0.9235 -> 0.9803、server CPU が 195.74% -> 92.35% まで改善した。`apex_rudp` は delivery 差 0.0011 の範囲で、server CPU は LiteNetLib より約 10% 低く、unreliable p99 も短い。

## Game Server

| library | valid | delivery | forward_r | forward_u | return_r | return_u | server CPU | rtt_r p99 | rtt_u p99 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `litenetlib` | 3/3 | 0.9818 | 1.0000 | 0.9907 | 1.0000 | 0.9898 | 23.69% | 221.1ms | 85.2ms |
| `gns` | 3/3 | 0.9805 | 1.0000 | 0.9896 | 1.0000 | 0.9898 | 160.00% | 228.6ms | 118.4ms |
| `apex_rudp` | 3/3 | 0.9802 | 1.0000 | 0.9868 | 1.0000 | 0.9899 | 72.34% | 154.0ms | 107.0ms |
| `enet` | 3/3 | 0.9252 | 1.0000 | 0.9891 | 1.0000 | 0.9316 | 41.07% | 257.9ms | 74.3ms |

Game server は delivery では LiteNetLib / GNS / apex がほぼ横並び。LiteNetLib は adapter 修正後に server CPU が 111.96% -> 23.69% まで下がった。`apex_rudp` は reliable event の p99 が最短で、delivery は top から 0.0016 差。

## Conclusion

- 本プロジェクトの final benchmark は `media_relay` と `game_server` の 2 production profiles とする。
- LiteNetLib の旧 CPU / delivery 差は adapter 実装由来の影響が大きく、broadcast hot path 修正後は production profile の上位に戻った。
- `apex_rudp` は media relay で LiteNetLib と delivery 同等かつ server CPU / unreliable p99 が低く、game server では reliable event p99 が最短。
- synthetic echo baseline で確認した 1000conn mixed 50/50 の強さは、production profile では「apex が優秀だが、LiteNetLib も adapter を適切に書けばかなり強い」という結果になった。

## Data

- profile 定義: [`data/profiles.csv`](data/profiles.csv)
- 中央値: [`data/summary.csv`](data/summary.csv)
- run 別: [`data/results_all.csv`](data/results_all.csv)
- scenario metadata: [`data/scenarios_all.csv`](data/scenarios_all.csv)
- raw run: `results/production_profiles_final_20260602/` + LiteNetLib remeasure `results/litenetlib_adapter_opt_prod_n3_20260602T080650Z/`（`results/` は gitignore）
