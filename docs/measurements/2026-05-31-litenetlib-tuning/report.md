# litenetlib 徹底チューニング — client 測定コスト除去で真の server 天井を解放

**測定日:** 2026-05-31
**目的:** litenetlib(現状の絶対王者、1000conn 0.994)を限界までチューニングする。分析で「天井は server でなく client の per-conn 2スレッドの測定コスト」と判明していたので、まず client を manual mode 化して測定を解放し、その上で server 設定を掃いて真の崩壊点を探る。

## セットアップ

- v2/v4 同条件: 隔離 server=7,15 / client=5,6,13,14 procs=4、netem 25/5/1(limit=100000)、mixed 50/50、size=64、idle=adaptive、**N=3 中央値**。warmup=3/duration=15(反復用)。
- LiteNetLib 設定を **env 変数化**(`LNL_MANUAL/IPV6/POOL/UPDATE/NATIVE` + `DOTNET_gcServer`)し 1ビルドで多設定を掃くリグ(`/tmp/lnlbench.sh`)。

## 核心: client manual mode が測定を解放

各 client conn が独自 NetManager を持ち、それぞれ **ReceiveThread + LogicThread の2 OS スレッド/conn** を起動する。1000conn = ~2000スレッドが 2物理コアを thrash → **client が律速**(load generator が負荷を出し切れない)。これが「litenetlib は client 3物理コア必要」「1000conn が valid 測定できない」の正体だった。

| @1000conn (2物理コア client) | dr | attempted_ratio | tick_ok |
|---|---|---|---|
| threaded (従来) | 0.997 | **0.88-0.91** | **0 (INVALID)** |
| **manual (StartInManualMode)** | 0.996 | **1.0000** | **1 (VALID 3/3)** |

`StartInManualMode` で per-conn スレッドを消し、`PollEvents()`(manual では受信も実行)+ gated `ManualUpdate()` で論理を main ループから駆動 → client は C++ load generator と同じ poll 方式に。**1000conn が 2物理コアで初めて valid 化**し、litenetlib の本当の server スケールが見えるようになった。

## server の真のスケール(manual client, default設定)

| conns | dr | server CPU% | valid |
|---|---|---|---|
| 1000 | 0.996 | 194 | ✓ |
| 1500 | 0.991 | 193 | ✓ |
| 2000 | 0.957 | 188 | ✓ (CPU 未飽和なのに低下→設定余地) |
| 3000 | (0.94-0.96) | 191 | ✗ (client att 0.97、server じゃなく client 律速) |

1000で頭打ちに見えていたのは client アーティファクトで、**実際は 2000conn まで捌く**。2000で CPU 188%(未飽和)なのに 0.957 に落ちる → server 設定の余地。

## server 設定スイープ(@2000, manual, baseline=0.957)

| config | dr@2000 | 備考 |
|---|---|---|
| baseline | 0.957 | PacketPoolSize=1000 枯渇 |
| ipv6off | 0.974 | dual-bind select 除去 |
| pool=4000 | 0.989 | **プール枯渇が主因** |
| native | 0.969 | + rtt timestamp 破損 → 却下 |
| gcServer | 0.986 | 単体は可だが combine で逆効果 → 却下 |
| **ipv6off + pool=4000** | **0.9965** | **最良** |
| all-on | 0.992 | native/gc が S5 を下げる |

- **PacketPoolSize=1000 の枯渇が崩壊主因**: 2000conn では in-flight が 1000 を超え、プール外 `new` → GC 圧。**2x conns に自動スケールで 0.957→0.989**。
- **IPv6 dual-bind 無効化**(IPv4 ベンチなので純粋な無駄)を重ねて **0.957→0.9965**。
- native/gcServer は却下(native は rtt timestamp を破損、gcServer は combine で逆効果)。

## 最終(shipped デフォルト: manual + ipv6off + pool自動スケール, env無し)

| conns | dr | valid |
|---|---|---|
| 1000 | **0.995** | ✓ |
| 2000 | **0.992** | ✓ |
| 3000 | ~0.95 | ✗(client att 0.97 — server でなく 2コア client の poll 限界) |

## 結論(盛らない、でも今回は本物の勝ち)

1. **最大の成果は測定の解放**: client manual mode で **1000conn が invalid→valid(2物理コア)**。litenetlib の真のスケールを隠していた client アーティファクトを除去した。これは kcp の「痛み分け」と違い **clean win**。
2. **server 設定で 2000conn を全回復**: ipv6off + pool自動スケールで 2000conn 0.957→**0.992**。
3. **真の天井が判明**: litenetlib server は **~0.99 を 2000conn まで維持**(server CPU ~195%、1物理コア=2レーンをほぼ使い切る)。3000 でも server は ~0.95 を出しており、**真の server 崩壊点は 2000 超**——以前は 1000 すら測れなかったことを思えば大幅な前進。マルチスレ .NET threadpool の地力。
4. **却下した設定を明記**: native sockets(rtt破損)・Server GC(combineで逆効果)。

**残課題**: 3000+ の真の server 崩壊点を見るには client 物理コアを増やす(manual client が 2コアで 3000ソケット poll しきれず att 0.97)。rtt_r_p99 が高 conns 域で uint64 wraparound 表示になる集計の縁(delivery には無影響)も要修正。

## 生データ

リグ出力のみ(未保存)。最終 shipped 設定: `LNL_MANUAL=1, LNL_IPV6=0, LNL_POOL=auto(2x conns), LNL_NATIVE=0`。
