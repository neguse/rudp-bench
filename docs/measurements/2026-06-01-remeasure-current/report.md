# Current remeasurement: 2026-06-01

**測定日:** 2026-06-01
**目的:** 現状コードで `2026-05-31` 系の v3 再測定条件を再度回し、最新の delivery / CPU / RTT を固定する。

## セットアップ

- ホスト: Ryzen 7 PRO 5750GE。`scripts/bench_isolate.sh setup`。
- server CPU: `7,15`、client CPU: `5,6,13,14`。
- netem: `sudo scripts/netem.sh apply 25 5 1 100000`。
- traffic: mixed `rate-r=50` + `rate-u=50`、size=64、mode=echo、duration=20s、idle=adaptive。
- client: `--client-procs=4`。
- conns: 200 / 600 / 1000。
- N=3、`scripts/aggregate_runs.py --min-valid 2` で valid run の中央値を採用。

## グラフ

- [`plots/delivery_vs_conns.png`](plots/delivery_vs_conns.png)
- [`plots/server_cpu_vs_conns.png`](plots/server_cpu_vs_conns.png)
- [`plots/rtt_p99_vs_conns.png`](plots/rtt_p99_vs_conns.png)

## 結果

| lib | 200 | 600 | 1000 |
|---|---:|---:|---:|
| enet | 0.9894 | 0.9884 | 0.8658 |
| kcp | 0.9899 | 0.8883 | 0.7146 |
| mini_rudp | 0.9899 | 0.7264 | ✗ server_crash |
| gns | 0.9901 | 0.9901 | 0.8549 |
| msquic | 0.5909 | ✗ valid 1/3 | ✗ client_crash |
| litenetlib | 0.9899 | 0.9900 | 0.9900 |

## メモ

- `litenetlib` は 1000conn まで valid で delivery 0.99 を維持。
- `enet` は今回 1000conn でも 0.866 まで残った。前回 v3 より高く、飽和域の run-to-run / session 差が大きい点は引き続き注意。
- `msquic 600` は1本だけ valid のため、aggregate としては invalid。
- `mini_rudp 1000` は `server_crash`。

## 生データ

中央値は [`data/summary.csv`](data/summary.csv)。raw run は `results/remeasure_current_20260601_205210/`（`results/` は gitignore）。
