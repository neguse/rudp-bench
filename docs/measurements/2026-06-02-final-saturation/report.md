# Final saturation profiles benchmark

> Superseded for the canonical final output by the apex batch remeasure:
> [`../2026-06-03-apex-batch-final/report.md`](../2026-06-03-apex-batch-final/report.md).
> This report remains the baseline multi-library run used for comparison.

**測定日:** 2026-06-02

**位置づけ:** 本プロジェクトの最終アウトプット。固定 conn の比較ではなく、各 workload の traffic shape を固定し、connection count を壊れるまで上げる。

## Break Rule

各 point は N=3。`scripts/aggregate_runs.py --min-valid 2` で valid run の中央値を採用する。

`OK` 条件:

- aggregate `valid=1`。
- median `delivery_ratio >= 0.95`。

最初に OK 条件を満たさなかった conn を `break_conns` とする。`client_tick` は load generator がその offered load を維持できなかったことを示すため、この同一ホスト測定では実用上の break として扱う。

## Profiles

| profile | mode | rate_r | rate_u | size | conns schedule | client procs | use case |
|---|---|---:|---:|---:|---|---:|---|
| `media_relay` | broadcast | 0 | 30 | 1000 | 50 75 100 125 150 200 | 1 | media SFU / relay fanout |
| `game_server` | broadcast | 1 | 20 | 128 | 64 96 128 192 256 | 1 | authoritative game state/event fanout |
| `echo` | echo | 50 | 50 | 64 | 200 600 1000 1500 2000 3000 | 4 | mixed synthetic baseline |

Setup は Ryzen 7 PRO 5750GE、server CPU `7,15`、client CPU `5,6,13,14`、netem `25ms +/- 5ms, 1% loss, rate 100000`、duration 20s、tail 500ms、idle `adaptive`。

## Capacity

| profile | library | max OK conns | break conns | break reason | last OK delivery | last OK server CPU | break delivery | break CPU |
|---|---|---:|---:|---|---:|---:|---:|---:|
| `media_relay` | `apex_rudp` | 50 | 75 | delivery<0.95 | 0.9798 | 82.65% | 0.4874 | 120.68% |
| `media_relay` | `litenetlib` | 50 | 75 | valid 1/3 | 0.9804 | 94.24% | 0.3075 | 120.33% |
| `media_relay` | `enet` | - | 50 | delivery<0.95 | - | - | 0.9467 | 77.09% |
| `media_relay` | `gns` | - | 50 | delivery<0.95 | - | - | 0.1529 | 85.26% |
| `game_server` | `litenetlib` | 96 | 128 | client_tick | 0.9809 | 38.47% | - | - |
| `game_server` | `apex_rudp` | 64 | 96 | delivery<0.95 | 0.9803 | 73.90% | 0.4613 | 134.54% |
| `game_server` | `gns` | 64 | 96 | valid 1/3 | 0.9810 | 160.87% | 0.4175 | 164.96% |
| `game_server` | `enet` | - | 64 | delivery<0.95 | - | - | 0.9273 | 42.28% |
| `echo` | `litenetlib` | 2000 | 3000 | client_tick | 0.9876 | 170.92% | - | - |
| `echo` | `apex_rudp` | 1000 | 1500 | client_tick | 0.9900 | 114.68% | - | - |
| `echo` | `enet` | 600 | 1000 | delivery<0.95 | 0.9882 | 93.52% | 0.8688 | 96.11% |
| `echo` | `gns` | 600 | 1000 | delivery<0.95 | 0.9899 | 161.83% | 0.8406 | 183.38% |

## Readout

- `media_relay`: full-room media fanout は 50 conn までが実用域。75 conn では apex / LiteNetLib とも delivery が大きく落ちる。ENet は 50 conn 時点で 0.9467 と閾値をわずかに下回り、GNS は media fanout に適さない。
- `game_server`: LiteNetLib が 96 conn まで維持し、128 conn で client_tick break。apex と GNS は 64 conn まで、96 conn で崩れる。ENet は 64 conn 時点で threshold 未満。
- `echo`: LiteNetLib が 2000 conn まで到達。apex は 1000 conn まで delivery 0.9900 を維持し、1500 conn で client_tick break。ENet / GNS は 600 conn まで。

## Conclusion

- 最終アウトプットは `media_relay` / `game_server` / `echo` の 3 saturation profiles とする。
- fixed-conn profile だけでは見えなかった「どこで壊れるか」が主指標になった。
- LiteNetLib は adapter hot path 修正後、game / echo の capacity で最も強い。apex は media 50 conn と echo 1000 conn で強いが、broadcast high-conns では 75/96 conn で崩れる。

## Data

- profile 定義: [`data/profiles.csv`](data/profiles.csv)
- capacity: [`data/capacity.csv`](data/capacity.csv)
- 中央値: [`data/summary.csv`](data/summary.csv)
- run 別: [`data/results_all.csv`](data/results_all.csv)
- scenario metadata: [`data/scenarios_all.csv`](data/scenarios_all.csv)
- raw run: `results/final_saturation_profiles_20260602/`（`results/` は gitignore）
