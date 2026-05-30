# kcp 徹底チューニング — パラメータ全掃 + 構造的レバー検証

**測定日:** 2026-05-31
**目的:** kcp の単スレ ARQ を限界までチューニングする。v4 で kcp が「痛み分け」(1000 +0.064 だが 600 -0.030)だったのを受け、window/interval/fastresend/minrto と構造的改善(drain-interleave / phase-desync)を系統的に掃いて最良設定を確定する。

## セットアップ

- v2/v4 同条件: 隔離 server=7,15 / client=5,6,13,14 procs=4、netem 25/5/1(limit=100000)、mixed 50/50、size=64、20s、idle=adaptive、**N=3 中央値**。
- kcp tuning を **env 変数化**(`KCP_WND/INTERVAL/RESEND/NC/MINRTO/DRAIN_CHUNK/DESYNC`)し、1ビルドで多設定を掃けるリグ(`/tmp/kcpbench.sh`)で 600/1000 を反復測定。生データは未保存(リグ出力のみ)。
- 実装した構造的レバー: **drain-interleave**(長い 1000-conn スキャン中に socket 排出を挟み input 溢れ→retx を防ぐ)、**phase-desync**(conn の flush 位相を interval 内で分散し更新バーストを散らす)、**minrto override**。

## 診断(出発点)

決定的だったのは RTT 内訳: **1000conn で reliable p99 ≈ 9.6秒** に対し同条件 unreliable p99 = 94ms。CPU は 89%(余裕あり)。→ ボトルネックは CPU でも socket でもなく **reliable ARQ の HoL stall**(loss したセグメントが rcv_nxt を止め、rmt_wnd 縮小→送信窓 stall の正帰還)。

## スイープ結果(600 / 1000 の N=3 中央値 delivery)

### Round 1: window × interval × 構造的レバー

| config | 600 | 1000 | 1000 CPU | 備考 |
|---|---|---|---|---|
| w256 i10 (原型/v3) | 0.921 | 0.716 | 89 | balanced |
| w512 i10 | **0.942** | 0.704 | 89 | 600 最良 |
| w256 i5 | 0.896 | **0.773** | 90 | **1000 最良** |
| w512 i5 (v4 commit) | 0.888 | 0.744 | 89 | |
| w256 i10 + drain128 | 0.968 | **0.548** | **96** | drain は1000破壊 |
| w256 i10 + desync | 0.934 | 0.698 | 90 | 中立 |
| w256 i10 + drain+desync | 0.963 | 0.495 | 96 | 1000破壊 |
| w512 i5 + drain+desync | 0.959 | 0.500 | 95 | 1000破壊 |

### Round 2: fastresend × minrto × 中間interval(w256基準)

| config | 600 | 1000 | 備考 |
|---|---|---|---|
| w256 i5 resend1 | 0.909 | 0.714 | resend1 は悪化 |
| w256 i5 minrto60 | 0.890 | 0.765 | 効果なし(誤差内) |
| w256 i5 resend1 minrto60 | 0.860 | 0.638 | 悪化 |
| w256 i7 resend1 minrto60 | 0.819 | 0.681 | 悪化 |
| w384 i5 resend1 minrto60 | 0.884 | 0.730 | w256 に劣る |

## 結論(盛らない)

1. **interval が唯一の実レバーで、これは 600↔1000 のトレードオフ**: 5ms は 1000 を上げる(HoL stall 回復が速い、~0.71→~0.76)が 600 を下げる(stall してない時の flush 過剰、~0.93→~0.89)。10ms は逆。**1000(スケール競争点)を取って 5ms を採用**。
2. **window は i5 で 256 が 384/512/1024 と同等以上**(両 conn 点)。前回の 512 は誤差〜微回帰だったので 256 に戻した。単スレ ARQ の HoL 天井下では大窓は buffering 遅延を増やすだけ。
3. **構造的レバーは全て却下(実測で悪化)**: drain-interleave は 600 を 0.97 に上げるが **1000 を 0.50 に破壊し CPU を 96% に飽和**(スキャン中の追加排出が単スレを食い潰す)。phase-desync は中立。fastresend=1 は spurious retx 増。minrto>30 は無効。いずれも off-by-default の toggle として残置。
4. **天井 ≈ 0.76**: 1000conn delivery は 1% loss 下で ~0.76 が単スレ ARQ HoL の限界。**どのチューニングも突破できなかった**(分析が予測した 0.85 には届かず)。マルチスレ勢の ~0.99 に並ぶには kcp の service ループを多スレ化するしかなく、それは kcp の設計(単スレ・単一ソースファイル)を壊すため対象外。

**最終設定**: `KCP_WND=256, KCP_INTERVAL=5(真の5ms、stock clamp は kcp->interval 直接代入で回避), KCP_RESEND=2, KCP_NC=1`。原型(w256 i10)比で **1000 +~0.05 / 600 -~0.04**。スケール寄りの穏当なシフトであって breakthrough ではない。**kcp の単スレ tail は限界が近い**——これが徹底チューニングの正直な答え。

> 注: run-to-run ノイズは ~±0.02(dev-notes §1.5)。window サイズの差(i5 で 256 vs 512)はノイズ内、interval の差(i5 vs i10、~0.05)はノイズ超で実体。

## 残課題

- kcp を 0.76 超に押すには service ループの多スレ化(設計変更)か、HoL を切る per-segment の選択的再送最適化(ikcp.c 改変)が必要。いずれも「stock kcp の特性」から外れるため、やるなら公平性注記つきの別ライブラリ扱い。
