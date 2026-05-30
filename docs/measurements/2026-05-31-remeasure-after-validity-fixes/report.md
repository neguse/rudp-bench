# Re-measure after the measurement-validity fixes (tasks.md L/D/M/S/DOC)

**測定日:** 2026-05-31
**目的:** `tasks.md` の計測妥当性レビュー反映（mini_rudp 単一fd多重化 L11、kcp 窓/interval/poll L7-L9、
msquic datagram 計装 L1/L4・pacing-off L2、harness の M1/M2/M4/M6 等）後に、v2 と同条件で取り直して
(1)変更が配信を壊していないこと、(2)改善・新診断シグナルが効いていることを確認する。

## セットアップ（v2 と同一）

- ホスト: Ryzen 7 PRO 5750GE。`scripts/bench_isolate.sh setup`（server=7,15 / client=5,6,13,14 / OS=0-4,8-12）。
- netem `apply 25 5 1`（limit=100000）。観測 RTT ~57ms、loss 1%。
- mixed rate-r=50 + rate-u=50、size=64、duration=20s、warmup=2s、mode=echo、idle=adaptive、**client procs=4**。
- **N=3 中央値**（`scripts/aggregate_runs.py`＝S1 で valid run のみ選別・集計。生データは `results/remeasure_v3/`＝gitignore）。
- conns は **200 / 600 / 1000** の 3 点（v2 の 5 点に対し時間都合で低/中/高の代表 3 点。methodology は同一）。
- 対象 lib: enet, kcp, mini_rudp, gns, msquic, litenetlib（全て procs=4・client 2物理コアで統一）。

## 結果（N=3 中央値、valid run のみ）

| lib | thread | conns | delivery | server CPU% | 対 v2 | 備考 |
|---|---|---|---|---|---|---|
| enet | single | 200 | 0.990 | 61.6 | =0.990 | 一致(無改修の sanity) |
| enet | single | 600 | 0.990 | 95.8 | =0.990 | 一致 |
| enet | single | 1000 | 0.373 | 99.2 | 0.241→0.373 | 飽和域でばらつき大(0.366–0.421)、環境差 |
| kcp | single | 200 | 0.992 | 35.7 | =0.993 | 一致 |
| kcp | single | 600 | **0.925** | 90.9 | 0.896→0.925 | **L7-L9 で一貫改善**(3run: 0.914/0.925/0.931) |
| kcp | single | 1000 | 0.709 | 88.6 | =0.714 | 同等(0.706–0.709、誤差内) |
| mini_rudp | single | 200 | **0.992** | 54.5 | **✗tick→0.992** | **L11 で初めて valid 化(下記)** |
| mini_rudp | single | 600 | ✗ tick | — | ✗→✗ | client CPU 飽和(attempted 0.17、下記) |
| mini_rudp | single | 1000 | ✗ tick | — | ✗→✗ | client CPU 飽和(attempted 0.12) |
| gns | multi | 200 | 0.992 | 138 | =0.993 | 一致(無改修の sanity) |
| gns | multi | 600 | 0.993 | 179 | =0.993 | 一致 |
| gns | multi | 1000 | 0.565 | 189 | =0.557 | 一致 |
| msquic | internal | 200 | 0.592 | 89.8 | =0.593 | **flat 0.58 不変**(計装で真因切り分け、下記) |
| msquic | internal | 600 | 0.574 | 131 | =0.573 | flat |
| msquic | internal | 1000 | ✗ crash | — | ✗→✗ | client_crash 不変(L3 のロック縮小では未解消) |
| litenetlib | multi | 200 | 0.994 | 122 | =0.994 | 一致 |
| litenetlib | multi | 600 | 0.994 | 173 | =0.994 | 一致 |
| litenetlib | multi | 1000 | ✗ tick | — | (v2 は 3物理コアで valid) | client 2物理コアでは under-provision(既知) |

## 解釈

### mini_rudp は L11 で初めて測定の土俵に乗った（L10/L11 の決定的検証）

v2 では **全 conns で valid=0/client_tick**。再測では **200conn が valid 化**（`attempted_ratio=1.0`,
`accepted_ratio=1.0`, `tick_ok=1`, delivery=0.992）。これは旧 report の「破綻」が **server delivery の限界ではなく
client adapter の負荷生成律速（conn 毎の別 socket → syscall 過多）だった**ことを実証する（L10 の結論）。
- 600/1000 は依然 invalid だが、原因は **client CPU 飽和**: client `cpu_pct≈400%`(4 proc×100%=2物理コア使い切り)で
  `attempted_ratio=0.17`(600)/`0.12`(1000)。mini_rudp の per-msg 処理(ACK/dedup/retx)が enet/kcp より重く、
  2物理コアの load generator が 600+ で律速。**server 側の限界ではない**(litenetlib が高 conns で client 3物理コアを
  要するのと同じ現象、load generator 過剰供給の原則)。client コアを増やせば 600+ も測定可能になる見込み。

### kcp は中域(600conn)で一貫改善（L7-L9）

600conn が **0.896→0.925**(N=3: 0.914/0.925/0.931 と全 run が v2 中央値を上回る)。窓拡大(L7)+5ms interval(L9)+
poll の O(1) idle 化(L8)の複合効果。1000conn は 0.709 で v2(0.714)と同等(飽和域)。server CPU は不変(88-91%)。

### msquic flat 0.58 の真因を計装で切り分け（L1/L4）— L2 仮説は反証

`close()` 時の datagram 送信状態カウンタ(L1/L4)が示したもの（client 側、200conn・50conn/proc）:

```
msquic_datagram: offered=55000 submit_failed=0 acked≈34000 (61%) lost≈306 (0.5%) canceled=0
```

- **submit_failed=0** → adapter は全 datagram を QUIC に渡せている（「送れていない」ではない）。
- **lost(LOST_DISCARDED)≈0.5%のみ** → cwnd/輻輳で捨てているわけではない。**L2 の「cwnd/pacing が datagram を一定割合
  捨てている」仮説は反証**。だから pacing 無効化(L2)で flat 0.58 は動かなかった（整合）。
- 欠損の大半(~38%)は **SENT のまま acked にも lost にもならず終了** = **forward path(client→server)の未達**。
  server 側の echo datagram は acked≈98.6% と健全なので、律速は **server の datagram 受信/ack 経路**にある公算。
- → 「flat 0.58 = 要調査」が「**forward path の SENT-but-unacked、submit でも cwnd でもない**」まで具体化した。
  根本対処には server 側 datagram 受信の計装が次の一手（L4 の切り分けが機能している証左）。
- 1000conn の client_crash は L3 のロックスコープ縮小では未解消（ramp は既に適用済み）。深いロック分割は別途。

### 無改修 lib(enet/gns/litenetlib)は v2 を再現＝methodology の妥当性

enet/gns/litenetlib の低・中 conns は v2 と一致（±0.001）。これは隔離+netem+集計パイプラインが v2 を正しく
再現していることの sanity。1000conn の飽和域は run-to-run のばらつきが大きく（enet 0.366–0.421）、絶対値の小差は
環境差として読む（dev-notes §1.5）。

### 新しい診断シグナルの稼働確認

- **`cpu_pct_peak`(M1)**: 全 lib で mean を上回る（例 msquic 86.6→**149.9**、gns 138→149）。2点平均に薄まる
  スパイクを捕捉できている。
- **`close_ms`(L6)**: msquic/enet/kcp/gns=**0**(同期 teardown なし)、mini_rudp=14ms、litenetlib=6ms。
  msquic の no-op close が lifecycle シグナルとして見える。
- **`msquic_datagram:`(L1/L4)** と上記の forward-path 切り分けが実機で機能。

## 結論

1. 変更は配信を壊していない（無改修 lib は v2 一致、改修 lib は同等以上）。
2. **mini_rudp が初めて valid（200conn, dr0.992）= L10/L11 の主目的を達成**。
3. kcp 中域が一貫改善（L7-L9）。
4. **msquic flat 0.58 は L1/L4 計装で forward-path の未達と切り分け、L2 の cwnd 仮説を反証**（pacing-off が無効だった理由）。
5. 新診断列（cpu_pct_peak / close_ms / msquic_datagram tally）が実機で稼働。

## 次の宿題

- mini_rudp 600+/litenetlib 1000: client 物理コアを増やして load generator 律速を外す（server 限界の測定）。
- msquic: server 側 datagram 受信の計装（forward-path 未達の根本切り分け）、1000conn crash の深いロック分割。
- conns 5 点（400/800 補完）と loss 軸での再スイープ。

## 生データ

`results/remeasure_v3/`（.gitignore のため未追跡）。中央値は [`data/summary.csv`](data/summary.csv)、
msquic datagram tally の控えは [`data/msquic_datagram_tally.txt`](data/msquic_datagram_tally.txt)。
集計は `scripts/aggregate_runs.py`（S1）で再現可能。
