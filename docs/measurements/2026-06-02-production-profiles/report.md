# Production profiles final benchmark

**測定日:** 2026-06-02

**位置づけ:** 本プロジェクトの最終アウトプット。`echo` は実装検証・合成 baseline とし、最終評価は media / game server の本番想定 profile で行う。

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

## Media Relay

| library | valid | delivery | forward_u | return_u | server CPU | rtt_u p99 |
|---|---:|---:|---:|---:|---:|---:|
| `apex_rudp` | 3/3 | 0.9792 | 0.9858 | 0.9900 | 83.03% | 75.2ms |
| `enet` | 3/3 | 0.9486 | 0.9876 | 0.9605 | 76.22% | 76.7ms |
| `litenetlib` | 3/3 | 0.9235 | 0.9900 | 0.9323 | 195.74% | overflow |
| `gns` | 3/3 | 0.1530 | 0.9903 | 0.9036 | 84.72% | 2136.0ms |

Media relay は `apex_rudp` が delivery で首位。server forward は各 adapter とも netem 1% loss 付近まで届くが、return path で差が出た。LiteNetLib は delivery が 0.9235 まで落ち、server CPU も apex の約 2.36 倍。LiteNetLib の media RTT は raw bin 上で timestamp underflow と見える値になったため、latency ranking には使わない。

## Game Server

| library | valid | delivery | forward_r | forward_u | return_r | return_u | server CPU | rtt_r p99 | rtt_u p99 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `litenetlib` | 3/3 | 0.9806 | 1.0000 | 0.9898 | 1.0000 | 0.9898 | 111.96% | 261.8ms | 83.5ms |
| `gns` | 3/3 | 0.9805 | 1.0000 | 0.9896 | 1.0000 | 0.9898 | 160.00% | 228.6ms | 118.4ms |
| `apex_rudp` | 3/3 | 0.9802 | 1.0000 | 0.9868 | 1.0000 | 0.9899 | 72.34% | 154.0ms | 107.0ms |
| `enet` | 3/3 | 0.9252 | 1.0000 | 0.9891 | 1.0000 | 0.9316 | 41.07% | 257.9ms | 74.3ms |

Game server は delivery では LiteNetLib / GNS / apex がほぼ横並び。`apex_rudp` は top delivery から 0.0004 差で、server CPU は LiteNetLib より 35% 低く、GNS より 55% 低い。reliable event の p99 も apex が最短。

## Conclusion

- 本プロジェクトの final benchmark は `media_relay` と `game_server` の 2 production profiles とする。
- `apex_rudp` は media relay で delivery 首位、game server で delivery 同等かつ CPU / reliable event latency で優位。
- synthetic echo baseline で確認した 1000conn mixed 50/50 の強さは、production profile でも media fanout と game fanout の両方に意味のある差として出た。

## Data

- profile 定義: [`data/profiles.csv`](data/profiles.csv)
- 中央値: [`data/summary.csv`](data/summary.csv)
- run 別: [`data/results_all.csv`](data/results_all.csv)
- scenario metadata: [`data/scenarios_all.csv`](data/scenarios_all.csv)
- raw run: `results/production_profiles_final_20260602/`（`results/` は gitignore）
