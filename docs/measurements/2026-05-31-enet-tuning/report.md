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

| @1000conn | dr | cpu |
|---|---|---|
| pool OFF | ~0.44 | 98 |
| **pool ON** | **~0.51** | 98 |

CPU は飽和のままだが、malloc に食われていた分が echo 処理に回り delivery が上がる。**+~0.08-0.11**(飽和点なので run-to-run ノイズ大: 0.48-0.55)。600 は未飽和で中立(0.990)。

## レバー2: socket 受信バッファ(env、既定OFF)

CPU 飽和中に socket queue が溢れて kernel drop している分を、大きいバッファで拾えるか。

| @1000 (pool ON) | dr |
|---|---|
| 256K(既定) | ~0.48-0.55 |
| 1MB | 0.550 |
| 4MB | 0.564 |

確かに効く(256K→4M で +~0.05-0.08)。**だが既定では採用しない**: enet だけ socket buffer を上げると L17 で統一した「全 UDP adapter 256KB の土俵」が崩れ、cross-lib 比較が不公平になる。`ENET_RCVBUF_KB` env で残置(全 adapter 一律に上げるなら別途)。

## 却下したレバー

- **recvmmsg/sendmmsg**(syscall バッチ化): 次の大きなレバーだが enet の socket 層(`unix.c`/`protocol.c`)の改変=**submodule fork** が必要で「stock enet」の線を越える(kcp の ikcp.c と同じ境界判断)。未採用。
- checksum/compression: 既に off(最適)。
- bandwidth/MTU/channel: 本ワークロードでは律速せず。

## 結論(盛らない)

- **採用 = プールアロケータ(fair な lib 内最適化、+~0.08-0.11 @1000)**。原型からの累計 **0.373 → 0.439(peerCount) → ~0.51(pool)** = +~0.14。
- enet は依然 **単スレ CPU 律速**(98%)。これは gns/litenetlib のような構造的勝ちではなく、単コアの天井(~0.51)は残る。
- socket buffer(不公平)と recvmmsg(submodule fork)は効くが、それぞれ公平性・stock 境界の理由で既定 OFF/未採用。**fair・in-API の範囲では ~0.51 が単スレ enet の到達点**。

## 残課題

- 全 UDP adapter を一律に大 socket buffer 化して公平に底上げ(L17 の再検討)。
- enet の真の上限を超えるには recvmmsg/sendmmsg(submodule fork、要公平性注記)。
