# gns — GameNetworkingSockets v2 server/client

benchspec/README.md 準拠の idiomatic な実装対。upstream の
`third_party/gns/examples/example_chat.cpp` のパターンに従う:
listen socket / `ConnectByIPAddress` + status-changed callback、
全接続を 1 つの poll group に集約、`RunCallbacks()` + 
`ReceiveMessagesOnPollGroup()` の polling ループ。

## thread model

app ロジック(受信 drain・echo/broadcast・metrics)は main スレッド 1 本。
status callback は `RunCallbacks()` から同期発火するため排他は不要。
パケット I/O・暗号・再送・タイマは GNS が生成する内部 service スレッドが
担う → `--describe` の thread_model は `internal_worker`。
GNS に blocking wait API はないため、service ループは 1ms 周期の polling
(client は次 slot の due 時刻まで、上限 1ms)。

## class mapping

| class | GNS 送信フラグ | 根拠 |
|---|---|---|
| loss-tolerant | `k_nSteamNetworkingSend_UnreliableNoNagle` | upstream が latency 重視の unreliable 送信向けに定義する複合フラグ(steamnetworkingtypes.h) |
| must-deliver | `k_nSteamNetworkingSend_Reliable` | Nagle(既定 5ms)は library 既定のまま |

- 暗号は GNS 既定で有効(AES-256-GCM + curve25519 鍵交換)。本ベンチの
  「暗号必須」代表として既定のまま計測する(`encryption: true`)
- 輻輳制御: SNP は送信レートを `SendRateMin`/`SendRateMax`(既定どちらも
  256KB/s)に clamp する token bucket。v1 adapter が行っていた
  SendRateMax/バッファ類の引き上げは v2 では行わない(library 既定)
- payload 上限: unreliable は `k_cbMaxUnreliableMsgSizeSend` = 16500B
  (1 パケット ~1200B を超えるとセグメント分割され、1 セグメント loss で
  メッセージ全体が落ちる)、reliable は 512KB。`--describe` の
  `max_payload_bytes` は両 class 共通の上限 16500 を報告する
- ビルドは Valve retail 相当の `_CERT` 定義付き(既定ビルドは Release でも
  debug assert / SNP paranoia が有効で計測に混入する)。`--describe` の
  tuning で開示

## build / test

```sh
cmake -S servers/gns -B build-v2-gns -DCMAKE_BUILD_TYPE=Release
cmake --build build-v2-gns -j
python3 servers/gns/smoke_test.py build-v2-gns/gns_server build-v2-gns/gns_client
go run ./orchestrator/cmd/orchestrator run -config orchestrator/examples/local-gns.json
```
