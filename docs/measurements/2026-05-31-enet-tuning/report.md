# enet 徹底チューニング — 単スレ CPU 飽和をプールアロケータで緩める

**測定日:** 2026-05-31
**目的:** 単スレッド enet を限界までチューニングする。enet は 1000conn で CPU 飽和(98%)が律速。peerCount 修正(全 peer スロット走査の固定税除去)は実施済み(原型 0.373→0.439)。残るのは per-message の CPU コスト削減。

## セットアップ

- v2/v4 同条件: 隔離 server=7,15 / client=5,6,13,14 procs=4、netem 25/5/1、mixed 50/50、size=64、20s、idle=adaptive、N=3 中央値。
- enet 設定を env 化(`ENET_POOL`/`ENET_RCVBUF_KB`)し 1ビルドで掃くリグ(`/tmp/enetbench.sh`)。

## 診断

enet は 1メッセージあたり ~3 回 malloc する(`ENetPacket` 構造体 + data バッファ + `ENetOutgoingCommand`)+ ACK ごとに `ENetAcknowledgement`。1000conn・~100k msg/s では malloc/free が秒間数十万回走り、**単スレ CPU を食い潰す**。checksum/compression は既に off(最適)。

## レバー1: プールアロケータ(採用)

`enet_initialize_with_callbacks` で glibc malloc を **thread-local の size 別フリーリスト**(O(1)、単スレベンチではロック無し)に置換。submodule 不変。size_t ヘッダで free 時のバケットを引く。

クリーン A/B(@1000、N=5、各 run):

| | runs | median | mean |
|---|---|---|---|
| pool OFF | 0.526/0.521/0.475/0.458/0.507 | 0.507 | 0.497 |
| **pool ON** | 0.565/0.757/0.521/0.491/0.648 | **0.565** | **0.596** |

pool ON は中心が高い(median +0.058、mean +0.099、ceiling 0.76)が**分布が飽和点ノイズと重なる**。理論的に正しく(malloc 削減)データも正方向だが、**精密な数字は主張できない(~+0.05-0.10、ノイズの縁)**。no-downside な健全変更として残置。600 は未飽和で中立(0.990)。

## レバー2: socket 受信バッファ → 【訂正: 効果はノイズだった】

当初「256K→4M で +~0.05-0.08 効く」と書いたが、これは**単一セッションの run 順による見せかけの trend**だった。同一セッションでのクリーン A/B(N=3-5、@1000)で検証すると:

| @1000 | dr |
|---|---|
| enet 256K | **0.589** |
| enet 1MB | **0.588** ← 差なし |

**enet にバッファ効果は無い**(飽和点の run-to-run 変動が ~0.45-0.59 と巨大で、当初の "trend" はノイズ)。さらに **kcp は 1MB で逆に大幅悪化**(0.781→0.521、ARQ の bufferbloat で spurious RTO retx 増、CPU 89→95%)。

→ **全 UDP adapter は 256KB 据え置きが正解**。litenetlib の内部 1MB も(buffer が効かない/kcp には有害な以上)実質的なアドバンテージではない。`ENET_RCVBUF_KB`/`KCP_RCVBUF_KB` env は今後の sweep 用に残置(既定 256)。dev-notes §1.5(N=1 は shape のみ、差は IQR 超で初めて言える)を地で踏んだ教訓。

## 却下したレバー

- **recvmmsg/sendmmsg**(syscall バッチ化): 次の大きなレバーだが enet の socket 層(`unix.c`/`protocol.c`)の改変=**submodule fork** が必要で「stock enet」の線を越える(kcp の ikcp.c と同じ境界判断)。未採用。
- checksum/compression: 既に off(最適)。
- bandwidth/MTU/channel: 本ワークロードでは律速せず。

## 結論(盛らない、訂正込み)

- **enet @1000 の飽和点はノイズが巨大(~0.45-0.59、±0.07-0.10)**。N=3-5 では ~0.05 級の効果を解像できない。これが最大の教訓 — 当初バッファで誤結論した。
- **採用 = プールアロケータ**: 理論的に健全(malloc 削減)でクリーン A/B も正方向(median +0.06、mean +0.10)だが**ノイズの縁**。精密な数字は主張せず、no-downside な改善として残置。
- **peerCount sizing(先行)**: 200conn の CPU が 61→45% に明確低下(これはノイズ超)= 固定スキャン税の除去は本物。1000 の delivery 寄与は飽和ノイズに埋もれる。
- **却下・訂正**: socket buffer 拡大は enet には無効(ノイズ)・kcp には有害(bufferbloat)→ 256KB 据え置き。recvmmsg は submodule fork で未採用。
- enet は依然 **単スレ CPU 律速**で、**fair・in-API の範囲での enet @1000 は ~0.5 前後**。gns/litenetlib のような構造的勝ちは無い。**「徹底的に調べた結果、確実な改善は peerCount の CPU 削減と健全な pool で、delivery の大幅押上げは飽和ノイズに阻まれて確認できない」が正直な答え**。

## 残課題

- 全 UDP adapter を一律に大 socket buffer 化して公平に底上げ(L17 の再検討)。
- enet の真の上限を超えるには recvmmsg/sendmmsg(submodule fork、要公平性注記)。
