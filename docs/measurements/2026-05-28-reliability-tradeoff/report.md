# Reliability tradeoff: unreliable vs reliable under loss

**測定日:** 2026-05-27
**目的:** loss 下で
1. 到達率を保つために RTT tail をどれだけ犠牲にするか(reliable vs unreliable 単独)
2. **reliable の retx が unreliable channel の RTT を汚すか(HoL leakage、混在ラン)**

**対象 lib:** raw_udp / mini_rudp / enet / gns / msquic

## セットアップ

- ホスト: Ryzen 7 PRO 5750GE、ARK・Minecraft 同居機。`scripts/bench_isolate.sh setup` で
  bench cores (6,7,14,15) を確保、`systemd-run --slice=bench-*.slice` 経由で server/client を分離
- ネット: `scripts/netem.sh apply 25 5 <loss>` (loopback。送/受両方で netem を通るため
  片道 25ms 指定 → RTT ~50ms、loss は実効 `1 - (1 - loss)^2`)
- client multi-proc farm: raw_udp/mini_rudp/enet は **N=4**、gns/msquic は
  multi-proc 起動で接続が安定しないため **N=1** で計測
- 共通: rate=50Hz、duration=20s、warmup=2s、idle=spin

| シナリオ prefix | size | reliable | unreliable | conns | loss | client procs | lib |
|---|---|---|---|---|---|---|---|
| `sw_l*_*`        | 100B | 0 Hz | 50 Hz | 10/50/100/200 | 1% / 5% | N=4 | raw_udp, mini_rudp, enet |
| `rel_l*_*`       | 64B  | 50 Hz | 0 Hz | 50 / 200      | 1% / 5% | N=4 | raw_udp, mini_rudp, enet |
| `unrel_n1_l*_*`  | 100B | 0 Hz | 50 Hz | 50 / 200      | 1% / 5% | N=1 | gns, msquic |
| `rel_n1_l*_*`    | 64B  | 50 Hz | 0 Hz | 50 / 200      | 1% / 5% | N=1 | gns, msquic |
| `mix_l*_*`       | 64B  | 50 Hz | 50 Hz | 50 / 200      | 1% / 5% | N=4 | mini_rudp, enet |
| `mix_n1_l*_*`    | 64B  | 50 Hz | 50 Hz | 50 / 200      | 1% / 5% | N=1 | gns, msquic |

`mix_*` は **両 channel を同時に 50Hz** で流して HoL を観測する条件。
raw_udp は reliable 非対応で混在不可なため除外。

(payload size が unrel=100B / rel=64B で揃ってないのは時系列の都合。1500B 以下では
RTT に効かないので比較自体は妥当)

## 結果

### Delivery ratio

![delivery_ratio](plots/delivery_ratio.png)

| conns | loss | mode | raw_udp | mini_rudp | enet | gns | msquic |
|------:|-----:|------|--------:|----------:|-----:|----:|-------:|
| 50  | 1% | unrel | 0.9823 | 0.9809 | 0.9808 | 0.9830 | 0.9831 |
| 50  | 1% | rel   | —      | 1.002  | 1.002  | 1.002  | 1.002  |
| 50  | 5% | unrel | 0.9054 | 0.9060 | 0.9028 | 0.9049 | ✗crash |
| 50  | 5% | rel   | —      | 1.002  | 1.004  | 1.004  | 1.003  |
| 200 | 1% | unrel | 0.9823 | 0.9826 | 0.9798 | 0.9823 | ✗crash |
| 200 | 1% | rel   | —      | **0.240 ✗FAIL** | 1.003 | 1.003 | ✗crash |
| 200 | 5% | unrel | 0.9034 | 0.9043 | 0.9025 | 0.8876 | ✗crash |
| 200 | 5% | rel   | —      | **0.233 ✗FAIL** | 1.004 | 1.004 | ✗crash |

- 動いた組は **unreliable は理論値 `(1-loss)²` ぴったり、reliable は dr ≈ 1.0**
- mini_rudp は 200 conn × reliable で破綻(client_tick FAIL、dr 0.24)
- msquic は 50conn 以上で頻繁に client_crash。**reliable 50conn だけが安定**
- raw_udp は構造的に reliable channel 非対応(`unsupported_reliable` で skip)

### RTT p99(retx tail)

![rtt_p99](plots/rtt_p99.png)

| conns | loss | mode | raw_udp | mini_rudp | enet | gns | msquic |
|------:|-----:|------|--------:|----------:|-----:|----:|-------:|
| 50  | 1% | unrel | 66.7 | 67.1 | 67.1 | 67.3 | 66.8 |
| 50  | 1% | rel   | —    | 102.1 | 127.1 | 175.2 | 119.5 |
| 50  | 5% | unrel | 66.7 | 67.1 | 67.1 | 67.5 | — |
| 50  | 5% | rel   | —    | 114.6 | 288.0 | 263.3 | 169.1 |
| 200 | 1% | unrel | 67.4 | 67.1 | 67.1 | 71.1 | — |
| 200 | 1% | rel   | —    | **17040 ✗** | 208.2 | 180.8 | — |
| 200 | 5% | unrel | 67.3 | 67.1 | 67.9 | 71.0 | — |
| 200 | 5% | rel   | —    | **16300 ✗** | 298.4 | 278.5 | — |

(値は ms。unreliable は loss に依らず ~67ms、reliable は retx で 100–300ms に
tail が伸びる)

### HoL leakage(本命)

reliable と unreliable を **同時に** 流したときに unreliable channel の RTT が
どれだけ汚れるかを比較する。`mix_*` の unreliable u99 を `sw_*` (unrel-only) と
並べる。

![hol_leakage](plots/hol_leakage.png)

| conns | loss | lib | unrel-only u99 | **mix u99** | HoL leak |
|------:|-----:|-----|---------------:|------------:|---------:|
| 50  | 1% | mini_rudp | 67.1 | 66.9 | ~0 |
| 50  | 1% | enet      | 67.1 | 67.1 | ~0 |
| 50  | 1% | gns       | 67.3 | 68.1 | +0.8 |
| 50  | 1% | msquic    | 66.8 | 67.2 | +0.4 |
| 50  | 5% | mini_rudp | 67.1 | 67.1 | ~0 |
| 50  | 5% | enet      | 67.1 | 67.1 | ~0 |
| 50  | 5% | gns       | 67.5 | 68.2 | +0.7 |
| 50  | 5% | msquic    | crash | 67.2 | (mix のみ動いた) |
| 200 | 1% | mini_rudp | 67.1 | **1115** | **+1048 (16.6×)** |
| 200 | 1% | enet      | 67.1 | 68.8 | +1.7 |
| 200 | 1% | gns       | 71.1 | 72.6 | +1.5 |
| 200 | 5% | mini_rudp | 67.1 | **702**  | **+635 (10.5×)** |
| 200 | 5% | enet      | 67.9 | 67.2 | ~0 |
| 200 | 5% | gns       | 71.0 | 72.5 | +1.5 |
| 200 | * | msquic    | crash | crash | — |

**結論:**
- enet / gns / msquic は **HoL leakage ほぼゼロ (<2ms)**。channel 分離きちんと動いている
- **mini_rudp は 200conn で massive HoL leakage**: reliable の retx が unreliable
  channel まで詰まらせ u99 が 10–17 倍。実装が channel ごとに pacing/queue を
  分けていないのが原因と思われる
- 50conn ではどの lib も HoL は表面化しない(負荷不足)

### Mixed delivery_ratio と RTT(参考)

| conn | loss | lib | dr | r99 (ms) | u99 (ms) | valid |
|-----:|-----:|-----|---:|---------:|---------:|-------|
| 50  | 1 | mini_rudp | 0.993 | 102 | 66.9 | ok |
| 50  | 1 | enet      | 0.990 | 127 | 67.1 | ok |
| 50  | 1 | gns       | 0.993 | 130 | 68.1 | ok |
| 50  | 1 | msquic    | 0.992 | 116 | 67.2 | ok |
| 50  | 5 | mini_rudp | 0.953 | 114 | 67.1 | ok |
| 50  | 5 | enet      | 0.954 | 282 | 67.1 | ok |
| 50  | 5 | gns       | 0.954 | 189 | 68.2 | ok |
| 50  | 5 | msquic    | 0.954 | 164 | 67.2 | ok |
| 200 | 1 | mini_rudp | 0.108 | 17800 | **1115** | tick_FAIL |
| 200 | 1 | enet      | 0.939 | 263   | 68.8 | tick_FAIL |
| 200 | 1 | gns       | 0.993 | 138   | 72.6 | tick_FAIL |
| 200 | 1 | msquic    | —     | —     | —    | crash |
| 200 | 5 | mini_rudp | 0.110 | 17647 | **702** | tick_FAIL |
| 200 | 5 | enet      | 0.925 | 516   | 67.2 | tick_FAIL |
| 200 | 5 | gns       | 0.954 | 199   | 72.5 | tick_FAIL |
| 200 | 5 | msquic    | —     | —     | —    | crash |

期待 dr = (1.0 + (1-loss)²) / 2 = `0.99 @ 1%`, `0.95 @ 5%` で一致。
200conn の tick_FAIL は厳密 budget overshoot で、dr/RTT 自体は実用域(mini_rudp 除く)。

### Unreliable RTT vs conns(scale invariance)

![unrel_scale](plots/unrel_scale.png)

raw_udp/mini_rudp/enet は conns 10→200 で p99 一定(±2ms)。
gns は 200conn で +4ms 上がる傾向(N=1 で 1 thread の処理コストが効いてる可能性)。

## 考察

### 1. unreliable は ライブラリ選択に依存しない
動いた 4 lib(raw_udp / mini_rudp / enet / gns)が **同じ `(1-loss)²` を出す**。
unreliable echo に対して RUDP lib の付加価値はゼロ — UDP 透過させているだけ。
unreliable で lib 比較したいなら **CPU 効率 / fan-out / framing オーバヘッド** など
別軸が必要。

### 2. reliable: 到達率を取り戻す代償は p99
4 lib の reliable @ 5% loss × 50conn を低 → 高 の順に並べると:

| lib       | r99 (ms) | retx 形 |
|-----------|----------|---------|
| mini_rudp | 114.6    | 単純 timeout retx だが軽い |
| msquic    | 169.1    | QUIC PTO + loss recovery |
| gns       | 263.3    | Steam Datagram の保守的 RTO |
| enet      | 288.0    | RTO ベース、conservative |

mini_rudp が一番速いが、これは 200 conn で破綻するように再送制御が
ナイーブで負荷耐性に欠ける(下記)。

### 3. ライブラリ別の上限(50Hz reliable)

| lib | 50 conn | 200 conn | 備考 |
|---|---|---|---|
| mini_rudp | OK (低 p99) | **破綻** (dr 0.24, p99 17s) | 再送が pacing を食う |
| enet      | OK | OK (p99 ~300ms) | retransmit policy 安定 |
| gns       | OK | OK (p99 ~280ms) | 安定 |
| msquic    | OK | crash | client 側で connection が安定せず |

**実装上の限界。**「reliable で 200 conn / 50Hz / 5% loss」が現実的に通るのは
**enet と gns の 2 つ**。msquic は単一クライアントから 200 conn 張る使い方に
チューニングが必要(connection multiplex の設定?)。

### 4. msquic crash パターン

| conn | loss | mode | msquic |
|------|------|------|--------|
| 50   | 1%   | unrel/rel | OK |
| 50   | 5%   | unrel | crash |
| 50   | 5%   | rel   | OK |
| 200  | *    | *     | crash |

5% loss × unreliable で stream 状態が乱れているように見える。
200 conn では mode を問わず client 起動直後に落ちる。
N=1 でも駄目なので multi-proc が原因ではない。別途調査が要る。

### 5. unreliable RTT は gns だけ conn でわずかに上がる
gns 200conn で +4ms。SteamNetworkingSockets 内部の per-conn 処理コストが N=1 では
顕在化している。raw/mini/enet は処理コストが安く影響しない。

### 6. HoL leakage は mini_rudp 固有(本命の結論)
混在 (rate-r=50 + rate-u=50) では:

- **enet / gns / msquic は HoL ほぼゼロ** — channel 設計が正しく機能している
- **mini_rudp は 200conn で u99 が 10–17 倍に膨れる** — reliable retx が
  unreliable channel まで詰まらせる。実装が共通 send queue / pacing なのが原因
- VoIP のような **unreliable レイテンシ厳守** 用途では:
  - mini_rudp の不利は決定的(自実装の限界、改善の余地大)
  - 他 3 lib はどれも安心して併用可能

## Caveat

- size mismatch(unrel 100B vs rel 64B)— payload < MTU 域では RTT に効かない想定
- bidirectional netem — RTT 観測値は片道 25ms × 2 + jitter
- mini_rudp の破綻は実装上の限界であって reliable 一般の限界ではない
- msquic の crash は N=1 でも発生するので multi-proc が原因ではない(別チケット)
- combined RTT histogram の罠は per-channel hist で回避済み

## 生データ

`./data/` 配下に raw CSV を保存。
プロット再生成は `./make_plots.py` を直接実行(matplotlib + pandas 必要)。

## 関連

- combined RTT 罠の解明: commit `085cefc` 以降
- pacing 閾値の経緯: commit `c0853eb`, `4eb941d`
- multi-proc client farm: commit `a0f4156`
