# apex_rudp: custom RUDP implementation benchmark

**測定日:** 2026-06-02

**目的:** 既存測定と sample adapter の知見をもとに、独自 RUDP adapter `apex_rudp` を実装し、現行の mixed 50/50 条件に投入する。

## 実装

- main data path は process ごとに UDP socket を共有し、wire の `conv` で logical connection を多重化。
- reliable は unordered delivery。64-bit SACK bitmap で ACK し、HoL を作らない。
- ACK は reliable packet に piggyback。単発受信向けに delayed standalone ACK も持つ。
- client から server reliable echo への ACK-only は server `port+1` の control socket に分離し、main data socket の queue を守る。
- 送信は `sendmmsg` batch。reliable の初回送信/再送は pending buffer を参照して余分な送信コピーを避ける。
- server の unreliable echo は専用 TX worker に逃がす。unreliable packet は owned buffer を worker queue に move し、reliable/ACK は direct path のままにして余分な reliable copy を避ける。
- socket buffer は `APEX_RCVBUF_KB` で指定可能、既定 4096KB。
- 1000conn の再送負荷を抑えるため RTO は 100ms。

## セットアップ

- ホスト: Ryzen 7 PRO 5750GE。`scripts/bench_isolate.sh setup`。
- server CPU: `7,15`、client CPU: `5,6,13,14`。
- netem: `sudo scripts/netem.sh apply 25 5 1 100000`。
- traffic: mixed `rate-r=50` + `rate-u=50`、size=64、mode=echo、duration=20s、idle=adaptive。
- client: `--client-procs=4`。
- conns: 200 / 600 / 1000。
- N=3、`scripts/aggregate_runs.py --min-valid 2` で valid run の中央値を採用。

## 結果

| conns | valid | delivery | forward_r | forward_u | return_r | return_u | server CPU | rtt_r p99 | rtt_u p99 |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 200 | 3/3 | 0.9899 | 1.0000 | 0.9899 | 1.0000 | 0.9899 | 45.01% | 151.0ms | 66.9ms |
| 600 | 3/3 | 0.9901 | 1.0000 | 0.9900 | 1.0000 | 0.9901 | 97.16% | 152.3ms | 68.7ms |
| 1000 | 3/3 | 0.9901 | 1.0000 | 0.9900 | 1.0000 | 0.9899 | 112.96% | 154.7ms | 69.6ms |

## 比較メモ

- 1000conn で LiteNetLib の直近中央値 `delivery=0.9900` / `server CPU=194.40%` に対し、apex は `delivery=0.9901` / `server CPU=112.96%`。
- 200/600/1000 全て 3/3 valid。delivery は netem 1% loss の理論上限にほぼ張り付き、1000conn の unreliable forward も 0.9900 まで回復した。
- `gns`/`enet`/`kcp` の直近 1000conn より delivery が高く、LiteNetLib と同等以上の delivery をより低い server CPU で出している。

## Tuning Log

- per-packet ACK から reliable-packet ACK piggyback へ寄せたが、ACK を絞りすぎると reliable 回復が悪化した。
- reliable の send path は pending buffer をそのまま batch に渡す形にして、通常送信の追加コピーを削った。無損失 1000conn では delivery 1.0000 / server CPU 61.14%。
- ACK-only を server `port+1` に分離したことで 1000conn の main data socket 圧迫が減り、旧 1000conn median 0.9173 から 0.9704 まで改善した。
- 最後の詰めは server unreliable echo 専用 TX worker。全送信 async は reliable copy と queue 競合で悪化したが、owned buffer の unreliable だけを move する worker は 1000conn median 0.9901 まで安定した。
- ACK-only を batch 化する案は 1000conn delivery 0.8952 に悪化したため不採用。
- 全送信 TX worker + async queue は client CPU の取り合い、server-only async は netem 1000conn で不安定化したため不採用。env `APEX_ASYNC_SEND=1|server|client` は実験口として残したが、既定では unreliable 専用 worker だけを使う。
- `sendmmsg` batch は 1000conn valid 化に効いた。batch 32 は悪化、batch 64 を採用。
- socket buffer 256KB では batch burst の unreliable drop が大きく、既定を 4096KB にした。
- RTO 150ms は 3/3 valid だが delivery 0.9003 に低下。RTO 100ms を採用。

## 生データ

- 中央値: [`data/summary.csv`](data/summary.csv)
- run 別: [`data/results_200_600.csv`](data/results_200_600.csv), [`data/results_1000.csv`](data/results_1000.csv)
- raw run: `results/apex_rudp_final_asyncu_netem_20260602/`（`results/` は gitignore）
