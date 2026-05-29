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
| msquic | 0.593 | 0.587 | 0.573 | 0.579 | ✗ crash |
| litenetlib | 0.993 | ✗ crash | ✗ crash | ✗ crash | ✗ crash |
| mini_rudp | ✗ tick | ✗ tick | ✗ tick | ✗ tick | ✗ tick |

### server CPU%（N=3 中央値、valid run のみ。server=1物理コア＝SMT 2スレッドで最大 ~200%）

| lib | 200 | 400 | 600 | 800 | 1000 |
|---|---|---|---|---|---|
| enet | 60 | 85 | 95 | 98 | 97 |
| kcp | 36 | 69 | 88 | 88 | 89 |
| gns | 137 | 158 | 174 | 182 | 184 |
| msquic | 84 | 115 | 122 | 124 | ✗ |
| litenetlib | 123 | ✗ | ✗ | ✗ | ✗ |

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
- **litenetlib**: 200conn のみ valid。400conn 以上は client_crash。**harness が litenetlib の multi-proc client farm に
  未対応（procs=1 固定）**で単一プロセスが高 conns で落ちる。litenetlib の真のスケールは未測定。

## 未解決・次の宿題

1. **litenetlib**: multi-proc client 対応 or 単一プロセスのクラッシュ原因究明（400conn で落ちる）。現状 200conn 超は測れない。
2. **msquic**: (a) 1000conn の client_crash、(b) delivery 一定 ~0.58 の原因（unreliable datagram の drop か flow control）。
3. **conns 上限の拡張**: enet/kcp/gns とも 600 までは差が出ない。差別化は 800-1000+ で出るので、gns/kcp の tail を見るには
   1500-2000conn まで延ばす価値あり（その際 client 2物理コアで attempted_ratio=1.0 を維持できるか要確認、必要なら client コア増）。

## 生データ

`results/scale_v2/`（.gitignore のため未追跡）。中央値は [`data/summary.csv`](data/summary.csv) に焼き込み。
設定・修正の根拠は [`../2026-05-30-netem-limit-artifact/report.md`](../2026-05-30-netem-limit-artifact/report.md)。
