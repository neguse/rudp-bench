# gns — GameNetworkingSockets v2 server/client(tuned)

benchspec/README.md 準拠の実装対。「ライブラリが想定する最速の使い方」を
ソース読解に基づいて設計した tune-to-plateau 版(方針: ルート CLAUDE.md)。
チューニングは全て upstream 公式ノブで、`--describe` の `tuning` に開示する。

## thread model

app ロジック(受信 drain・echo/broadcast・metrics)は main スレッド 1 本。
status callback は `RunCallbacks()` から同期発火するため排他は不要。
パケット I/O・暗号・再送・ACK・タイマは GNS の内部 service スレッド
**1 本**が全接続分を直列処理する(`steamnetworkingsockets_lowlevel.cpp:1826,
3521`)→ `--describe` の thread_model は `internal_worker`。

**ライブラリ固有のスケール上限**: 受信 decode・送信・再送が単一スレッドに
集約されるため、コネクション数を増やしてもコア方向にスケールしない。
poll group を分けても取り出し側の分散にしかならない(受信キュー投入は
単一ロック `g_lockAllRecvMessageQueues` で直列)。これは adapter では
削れない GNS 自体の律速点。

## イベントループ設計

- 受信は `ReceiveMessagesOnPollGroup`(取り出し O(msg 数)、conn 数に非依存、
  `connections.cpp:188-207`)を 256 msg × 最大 16 batch/呼び出しで drain。
  **batch 上限が必須**: broadcast fanout(受信 conns² スケール)では上限なし
  だと drain ループから抜けられず、送信 pacing と bk_steady_tick が飢える
  (`docs/ledger.md` #13 の機構)。受信・ACK は内部スレッドが継続するので
  残りは次呼び出しで引けばよい。
- server は受信が続いている間(直近 drain が満杯 batch)は sleep しない。
  固定 1ms sleep は app スレッド drain の throughput 上限を作る。
- GNS に blocking wait API はないため idle 時は 1ms 周期 polling。

## 送信経路(copy 削減)

- `SendMessageToConnection` は enqueue のたびに struct new + payload
  malloc + memcpy(`connections.cpp:2038,2044`)。
- **client**: `AllocateMessage(len)` のバッファに header を直接書き、
  `SendMessages` で所有権ごと渡す(enqueue copy 0、
  `isteamnetworkingsockets.h:270-274`)。
- **server broadcast**: GNS に同報 API は無い。`AllocateMessage(0)` で
  struct のみ N 個確保し、`m_pData` を refcount 付き共有バッファへ向けて
  `m_pfnFreeData` で寿命管理、`SendMessages` で一括投入する(payload copy
  N→1、同一 conn 連続分のロック取り直しも回避、
  `csteamnetworkingsockets.cpp:1338-1353`)。upstream が保証する最小コスト
  経路(`isteamnetworkingsockets.h:276-293`)。
- 暗号化のため符号化時の per-conn packet copy は残る(`snp.cpp:2188,2265`)。
  これはライブラリ仕様で削れない。

## class mapping

| class | GNS 送信フラグ |
|---|---|
| loss-tolerant | `k_nSteamNetworkingSend_UnreliableNoNagle` |
| must-deliver | `k_nSteamNetworkingSend_Reliable`(Nagle 既定 5ms のまま) |

- 暗号は GNS 既定で有効(AES-256-GCM + curve25519 鍵交換)。本ベンチの
  「暗号必須」代表として既定のまま計測する(`encryption: true`)
- payload 上限: unreliable は 16500B(超えると **reliable に自動変換**、
  `snp.cpp:329-333`。また ~1200B 超はセグメント分割され 1 セグメント loss で
  全体が落ちる)、reliable は 512KB
- 未検証の候補: 小 payload(≤200B)の broadcast fanout では lt を
  Nagle 有効(`k_nSteamNetworkingSend_Unreliable`)にすると同一 conn 宛の
  複数 msg が 1 パケットに coalesce され pps が下がる可能性(+最大 5ms
  レイテンシ)。バトル時の wired 測定で A/B する

## チューニング(`--describe` の `tuning` に開示)

| ノブ | 値(既定) | 根拠 |
|---|---|---|
| SendRateMin/Max | 256MB/s(256KB/s) | SNP の token bucket は unreliable も含む全送信をこの帯域に律速する(`snp.cpp:344-345,4189-4222`)。既定 256KB/s は fanout 需要の 1/10 以下で、旧実装の broadcast delivery 崩壊の主因 |
| SendBufferSize | 16MB(512KB) | 溢れると送信が `k_EResultLimitExceeded` で即失敗(`snp.cpp:321-326`) |
| RecvBufferSize / Messages | 64MB / 65536(1MB / 1000) | per-conn 上限。app drain が一瞬遅れると unreliable がここで破棄される(`connections.cpp:2760-2774`) |
| TimeoutConnected | 60s(10s) | 高負荷時の切断猶予(判定 `connections.cpp:3599-3634`) |
| `_CERT` compile definition | defined | Valve retail 相当(debug assert / SNP paranoia を無効化) |

## 確認結果(loopback 5s run、2026-07-10)

- echo c4: VALID delivery 1.000(変更前後で同等)
- broadcast c64(30Hz×1000B、期待 123k msg/s): delivery **0.086 → 0.943**、
  p50 sched 2.1s → 82ms(SendRate 解放 + 共有 broadcast + drain budget)
- broadcast c128: VALID delivery 0.276(正直な過負荷劣化、crash なし)

## build / test

```sh
cmake -S servers/gns -B build-v2-gns -DCMAKE_BUILD_TYPE=Release
cmake --build build-v2-gns -j
python3 servers/gns/smoke_test.py build-v2-gns/gns_server build-v2-gns/gns_client
go run ./orchestrator/cmd/orchestrator run -config orchestrator/examples/local-gns.json
```
