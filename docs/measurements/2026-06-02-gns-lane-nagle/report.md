# GNS lane/Nagle comparison: 2026-06-02

**測定日:** 2026-06-02
**目的:** GNS の reliable/unreliable を別 lane に分けた上で、Nagle あり/なしを比較する。

## セットアップ

- ホスト: Ryzen 7 PRO 5750GE。`scripts/bench_isolate.sh setup`。
- server CPU: `7,15`、client CPU: `5,6,13,14`。
- netem: `sudo scripts/netem.sh apply 25 5 1 100000`。
- traffic: mixed `rate-r=50` + `rate-u=50`、size=64、mode=echo、duration=20s、idle=adaptive。
- client: `--client-procs=4`。
- conns: 200 / 600 / 1000。
- N=3、`scripts/aggregate_runs.py --min-valid 2` で valid run の中央値を採用。

## 実装条件

- `gns_split_no_nagle`: reliable を lane 0、unreliable を lane 1 に分け、`k_nSteamNetworkingSend_NoNagle` を指定。
- `gns_split_nagle`: 同じ lane 分割で、GNS 既定の Nagle を使用。

どちらも `ConfigureConnectionLanes(hConn, 2, nullptr, nullptr)` で同 priority / equal share。lane 指定のため、送信は `AllocateMessage` + `SendMessages` を使う。

## グラフ

- [`plots/delivery_vs_conns.png`](plots/delivery_vs_conns.png)
- [`plots/forward_unreliable_vs_conns.png`](plots/forward_unreliable_vs_conns.png)
- [`plots/server_cpu_vs_conns.png`](plots/server_cpu_vs_conns.png)
- [`plots/rtt_p99_vs_conns.png`](plots/rtt_p99_vs_conns.png)

## 結果

| condition | conns | delivery | forward_u | server CPU | rtt_r p99 | rtt_u p99 |
|---|---:|---:|---:|---:|---:|---:|
| split + NoNagle | 200 | 0.9900 | 0.9900 | 93.39% | 149.8ms | 123.6ms |
| split + Nagle | 200 | 0.9900 | 0.9900 | 74.65% | 144.1ms | 89.7ms |
| split + NoNagle | 600 | 0.9900 | 0.9899 | 165.94% | 150.9ms | 99.0ms |
| split + Nagle | 600 | 0.9901 | 0.9900 | 152.40% | 179.2ms | 151.5ms |
| split + NoNagle | 1000 | 0.8514 | 0.7099 | 183.33% | 576.1ms | 173.2ms |
| split + Nagle | 1000 | 0.8544 | 0.7160 | 182.25% | 541.0ms | 178.9ms |

## 解釈

- 1000conn の delivery は `0.8514` vs `0.8544` で、Nagle ありの改善は +0.003 程度。run-to-run の範囲内で、delivery を押し上げる決定打ではない。
- 1000conn の forward unreliable も `0.7099` vs `0.7160`。直近 baseline の `gns` は `0.7169` なので、equal-share lane 分割はほぼ効いていない。
- 200/600conn では Nagle ありのほうが server CPU は下がる。ただし 600conn の RTT p99 は reliable/unreliable とも悪化する。
- つまり今回の条件では、GNS の 1000conn 崩れは「単一 lane」や `NoNagle` 単独では説明できない。reliable は引き続き 1.0 を維持し、unreliable client-to-server だけが落ちるため、GNS SNP 内部の reliable retry 優先、packet budget、server 側処理飽和が主因のまま。

## 生データ

- 中央値: [`data/summary.csv`](data/summary.csv)
- run 別結果: [`data/results_all.csv`](data/results_all.csv)
- scenario metadata: [`data/scenarios_all.csv`](data/scenarios_all.csv)
- raw run: `results/gns_lane_nagle_20260602_015230/`（`results/` は gitignore）
