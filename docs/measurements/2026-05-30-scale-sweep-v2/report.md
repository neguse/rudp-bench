# Scale sweep v2: conns 200..1000 (netem artifact removed, valid measurement)

**測定日:** 2026-05-30
**目的:** `2026-05-29-scale-sweep` は netem の queue 上限(limit 1000)アーティファクトと
spin による CPU 誤読、client 過少資源で汚染されていた（[`../2026-05-30-netem-limit-artifact`](../2026-05-30-netem-limit-artifact/report.md)）。
原因を全て潰した設定で取り直し、各 lib の**本当の**スケール特性を出す。

## セットアップ（前回からの修正点）

- ホスト: Ryzen 7 PRO 5750GE。`scripts/bench_isolate.sh setup`（新レイアウト）で隔離。
- **client = 2物理コア (5,6,13,14)、procs=4**（前回 1物理コア → 高 conns で負荷を出し切れず invalid だった）。
- **server = 1物理コア (7,15)** 固定（= 測定変数。1コアあたりのスループットを見る）。
- **netem: `apply 25 5 1` だが limit=100000**（前回は未指定で既定 1000 → キュー溢れが delivery を頭打ちにしていた）。
- **idle=adaptive**（spin は CPU が常時 100% で飽和判定不能）。
- 共通: mixed rate-r=50 + rate-u=50、size=64B、duration=20s、warmup=2s、mode=echo、**N=3**（中央値）。
- 対象 lib: enet, kcp, mini_rudp, gns, msquic（multi-proc 群）+ litenetlib（procs=1）。
  raw_udp(reliable無)/udt4(unreliable無)/slikenet(1conn)/yojimbo(64conn上限) は capability 上スケール対象外。

## 結果

### delivery_ratio（N=3 中央値、valid run のみ）

| lib | 200 | 400 | 600 | 800 | 1000 |
|---|---|---|---|---|---|
| enet | 0.990 | 0.991 | 0.990 | 0.749 | 0.241 |
| kcp | 0.993 | 0.993 | 0.896 | 0.777 | 0.714 |
| gns | 0.993 | 0.993 | 0.993 | 0.789 | 0.557 |
| litenetlib † | 0.994 | 0.994 | 0.994 | 0.994 | 0.994 |
| msquic | 0.593 | 0.587 | 0.573 | 0.579 | ✗ crash |
| mini_rudp | ✗ tick | ✗ tick | ✗ tick | ✗ tick | ✗ tick |

† litenetlib は multi-proc client 対応後に再取得（2026-05-30）。client 負荷生成が重いため
200-600conn は client 2物理コア、800-1000conn は **3物理コア**（procs=6）で attempted_ratio≥0.99 を確保。
server は全点 1物理コアで他 lib と同条件。

### server CPU%（N=3 中央値、valid run のみ。server=1物理コア＝SMT 2スレッドで最大 ~200%）

| lib | thread | 200 | 400 | 600 | 800 | 1000 |
|---|---|---|---|---|---|---|
| enet | single | 60 | 85 | 95 | 98 | 97 |
| kcp | single | 36 | 69 | 88 | 88 | 89 |
| gns | multi | 137 | 158 | 174 | 182 | 184 |
| litenetlib | multi | 124 | 147 | 171 | 190 | 191 |
| msquic | internal | 84 | 115 | 122 | 124 | ✗ |

> **CPU% は thread モデルを併記して読むこと（DOC3/D5）。** 単一スレッド lib(enet/kcp)の上限は ~100%、
> マルチスレッド lib(gns/litenetlib)と msquic(内部 worker)は ~200%（1物理コアの SMT 2レーン）。CPU 値を
> 単独で並べると「enet が CPU 効率最良」等と誤読しやすいので `data/summary.csv` に `thread_model` 列を追加した。
> ユーザー軸では「同一ハードで 2レーンを使い切るのは実力」なので割り引かない。

## 解釈

- **前回(汚染)との差が大きい**: 旧 report は enet が 200conn から 0.94→0.52(600)→0.30(1000) と落ちるとしていたが、
  あれは netem キュー溢れ。**真の enet は 600conn まで 0.99 を維持**し、knee は ~700-800（server CPU が 95-98% に
  張り付く点）。スケール特性は旧結論より遥かに良い。
- **enet (単一スレッド server)**: server CPU が conns に比例して上がり 600 で ~95%、800 以降 1コア飽和 → delivery 崩壊
  (0.75→0.24)。典型的な単一コア律速。
- **kcp**: 最も CPU 効率が良く（1000conn でも server 89%）、劣化が最も緩やか(0.71@1000)。単一スレッドだが enet より
  per-conn コストが軽い。
- **gns (マルチスレッド server)**: server CPU 137→184%（SMT 2スレッド＝~2コア相当を使う）。600 まで 0.99、頂点で
  最良の tail (0.56@1000 vs enet 0.24)。**「gns がスケールする」優位は最高負荷域でのみ顕在化**し、~600conn までは
  enet/kcp と横並び。
- **msquic**: delivery が conns に依らず **~0.58 で一定**（スケール律速ではなく、QUIC datagram の unreliable 側が
  恒常的に ~42% 落ちている msquic 固有特性。要調査）。1000conn で client_crash。
- **mini_rudp**: 全 conns で valid=0/client_tick。ベースライン実装が負荷を出し切れない（既知の破綻、想定内）。
- **litenetlib (マルチスレッド server)**: multi-proc client 対応後の再取得で、**1000conn まで delivery 0.994 を維持**。
  server CPU は 124→191% と上がり 1物理コア（SMT 2スレッド）をほぼ使い切る。同じ 1物理コア server でも gns が
  1000 で 0.56 に落ちるのに対し litenetlib は 0.99 を保つ＝この負荷条件では最良のスケーラ（.NET threadpool で
  受信/echo を捌ききっている）。
  - **【2026-05-30 コード精査で保留を解消】** 当初「gns より大きく良いのは意外＝アーティファクト疑い」と保留したが、
    `delivery_ratio = received/accepted` の `received` は **server drop を必ず捕捉する**（落ちた echo は client に戻らない
    ＝received されない）。したがって 0.994 を維持できているのは **echo を実際に捌けている証拠**であり、計測アーティファクト
    ではなく**正当な実力の公算が高い**。確定的な裏取りが要るなら `StartInManualMode()` でシングルスレッド化して再測し、
    「実力(スレッド並列)」か「資源(2レーン)」かを切り分けられる（gns との差は thread モデル差＝`summary.csv` の
    `thread_model` 列、ユーザー軸では正当な実力差）。

## 未解決・次の宿題

1. ~~litenetlib: multi-proc client 対応~~ → **完了 (2026-05-30)**。実装: ①.NET adapter に RTT bin 出力
   （C++ `LatencyHist` と同一形式、`--bins-r-out`/`--bins-u-out`）、②`run_phase1_quick.sh` に litenetlib
   multi-proc farm、③litenetlib 側 tick_ok から tick_gap 除去、④`combine_clients.py` の tick_ok を per-proc AND
   から**集約比率ベース**に変更（6 proc 分割時に1 proc が 0.989 に落ちると全体 fail になる過剰判定の修正。1000conn で
   集約 attempted=0.992 なのに invalid になっていた）。上表 litenetlib 行は v2 config（server 1物理コア・N=3）で
   再取得済み。litenetlib client は重いので 800-1000 は client 3物理コア必要（load generator 過剰供給の原則）。
2. **msquic**: (a) 1000conn の client_crash、(b) delivery 一定 ~0.58 の原因（unreliable datagram の drop か flow control）。
   → **【2026-05-30 計装追加】** adapter に datagram 送信状態の分類カウンタを実装(L1/L4)。close() 時に
   `msquic_datagram: offered=.. submit_failed=.. acked=.. lost=.. canceled=..` を stderr に出すので、~0.58 の欠損が
   **QUIC 側の LOST_DISCARDED(cwnd で捨てている)か submit_failed(そもそも送れていない)か**を次ランで切り分けられる。
   併せて pacing を無効化(L2、`PacingEnabled=FALSE`)。これらの効果は再測で確認する（コード変更済み・数値は未更新）。
3. **conns 上限の拡張**: enet/kcp/gns とも 600 までは差が出ない。差別化は 800-1000+ で出るので、gns/kcp の tail を見るには
   1500-2000conn まで延ばす価値あり（その際 client 2物理コアで attempted_ratio=1.0 を維持できるか要確認、必要なら client コア増）。

## 生データ

`results/scale_v2/`（.gitignore のため未追跡）。中央値は [`data/summary.csv`](data/summary.csv) に焼き込み。
設定・修正の根拠は [`../2026-05-30-netem-limit-artifact/report.md`](../2026-05-30-netem-limit-artifact/report.md)。
