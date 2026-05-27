# Reliability tradeoff: unreliable vs reliable under loss

**測定日:** 2026-05-27
**目的:** loss 下で「到達率を保つために RTT tail をどれだけ犠牲にするか」を unreliable / reliable の対比で可視化する。
**対象 lib:** raw_udp / mini_rudp / enet (gns・msquic は multi-proc client farm で未対応のため除外)

## セットアップ

- ホスト: Ryzen 7 PRO 5750GE、ARK・Minecraft 同居機。`scripts/bench_isolate.sh setup` で
  bench cores (6,7,14,15) を確保、`systemd-run --slice=bench-*.slice` 経由で server/client を分離
- ネット: `scripts/netem.sh apply 25 5 <loss>` (loopback。送/受両方で netem を通るため
  片道 25ms 指定 → RTT ~50ms、loss は実効 `1 - (1 - loss)^2`)
- client multi-proc farm: `--client-procs=4`、histogram は `combine_clients.py` でバイナリ
  マージ後に percentile 再計算
- 共通: rate=50Hz、duration=20s、warmup=2s、idle=spin

| シナリオ | size | reliable | unreliable | conns | loss |
|---|---|---|---|---|---|
| `sw_l*_*`  | 100B | 0 Hz | 50 Hz | 10/50/100/200 | 1% / 5% |
| `rel_l*_*` | 64B  | 50 Hz | 0 Hz | 50 / 200 | 1% / 5% |

(payload size が unrel=100B / rel=64B で揃ってないのは時系列の都合。1500B 以下では
RTT に効かないので比較自体は妥当)

## 結果

### Delivery ratio

![delivery_ratio](plots/delivery_ratio.png)

| conns | loss | mode | raw_udp | mini_rudp | enet | 期待値 |
|------:|-----:|------|--------:|----------:|-----:|-------:|
| 50 | 1% | unrel | 0.9823 | 0.9809 | 0.9808 | 0.9801 = (1-0.01)² |
| 50 | 1% | rel | — | **1.002** | **1.002** | 1.0 |
| 50 | 5% | unrel | 0.9054 | 0.9060 | 0.9028 | 0.9025 = (1-0.05)² |
| 50 | 5% | rel | — | **1.002** | **1.004** | 1.0 |
| 200 | 1% | unrel | 0.9823 | 0.9826 | 0.9798 | 0.9801 |
| 200 | 1% | rel | — | **0.240 ✗** | **1.003** | 1.0 |
| 200 | 5% | unrel | 0.9034 | 0.9043 | 0.9025 | 0.9025 |
| 200 | 5% | rel | — | **0.233 ✗** | **1.004** | 1.0 |

- unreliable は理論値 `(1-loss)²` とぴったり一致(双方向 netem の合算)
- reliable は dr ≈ 1.0(>1.0 は dedup overshoot 〜0.4%)
- mini_rudp は 200 conn 時に **dr 0.24 まで崩壊** → 再送実装が pacing を壊して
  client_tick FAIL
- raw_udp は reliable channel 非対応のため空欄

### RTT p99(retx tail)

![rtt_p99](plots/rtt_p99.png)

| conns | loss | mode | raw_udp p99 | mini_rudp p99 | enet p99 |
|------:|-----:|------|------------:|--------------:|---------:|
| 50 | 1% | unrel | 66.7 ms | 67.1 ms | 67.1 ms |
| 50 | 1% | rel | — | 102.1 ms | 127.1 ms |
| 50 | 5% | unrel | 66.7 ms | 67.1 ms | 67.1 ms |
| 50 | 5% | rel | — | 114.6 ms | 288.0 ms |
| 200 | 1% | unrel | 67.4 ms | 67.1 ms | 67.1 ms |
| 200 | 1% | rel | — | **17040 ms ✗** | 208.2 ms |
| 200 | 5% | unrel | 67.3 ms | 67.1 ms | 67.9 ms |
| 200 | 5% | rel | — | **16300 ms ✗** | 298.4 ms |

reliable のみ retx tail、unreliable はどの lib も loss 影響なし。

### Unreliable RTT vs conns(scale invariance)

![unrel_scale](plots/unrel_scale.png)

unreliable はどの lib も conns 10→200 で p99 一定(±2ms)。
conn 数による saturation シグナルなし → 200conn まで余裕。

## 考察

### 1. unreliable: lib 選択は到達率に効かない
3 lib(raw_udp / mini_rudp / enet)が **完全一致** で `(1-loss)²` を出す。
unreliable echo に対して RUDP lib が提供する付加価値はゼロ(構造的に当然 — UDP 透過させているだけ)。
unreliable で lib 比較したいなら **fan-out / framing CPU 効率** など別軸を取る必要がある。

### 2. reliable: 到達率を取り戻す代償は p99
- enet @ 50conn × 5% loss: u99 67ms → r99 288ms (+221ms)
- enet @ 200conn × 5% loss: u99 68ms → r99 298ms (+231ms)
- mini_rudp は 50conn なら同等(+47ms)、200conn で破綻

RTO ~50-100ms × 数回再送 = 200-300ms の "尻尾" が dr 1.0 のコスト。

### 3. mini_rudp の reliable は 50conn 程度が上限
200 conn × reliable で client_tick FAIL、dr 0.24、p99 17 秒。
再送実装 (現状: 単純 timeout retransmit) が pacing budget を食いつぶす。
**用途上限の目安: ~100 conn / 50Hz / reliable**。
enet と置き換える前提なら、再送の amortization (RACK 風 / Lazy ack) 検討が必要。

### 4. enet の reliable は 200conn × 5% loss で持つ
p99 ~300ms は VoIP には厳しいが、ゲーム RPC や状態同期(秒オーダーまで許容)では
許容範囲。「全部 reliable で押す」設計の現実解として 200conn は通る。

## Caveat

- size mismatch(unrel 100B vs rel 64B)— payload < MTU 域では RTT に効かない想定
- bidirectional netem — RTT 観測値は片道 25ms × 2 + jitter
- mini_rudp の破綻は実装上の限界であって reliable 一般の限界ではない
- combined RTT histogram の罠は per-channel hist で回避済み([[measurement_pitfalls]] 参照)

## 生データ

`./data/` 配下に raw CSV を保存。
プロット再生成は `./make_plots.py` を直接実行(matplotlib + pandas 必要)。

## 関連

- 前ラン(combined RTT 罠の解明): commit `085cefc` 以降
- pacing 閾値の経緯: commit `c0853eb`, `4eb941d`
- multi-proc client farm: commit `a0f4156`
