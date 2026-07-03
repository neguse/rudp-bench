# enet — ENet server/client

benchspec/README.md 準拠の idiomatic な実装対。単一スレッドの
`enet_host_service` ポーリングループ(`ENET_SERVICE_SLICE_MS`=10ms)で
接続受け入れ・受信 drain・echo/broadcast・metrics を処理する →
`--describe` の thread_model は `single`。

## class mapping

| class | channel | ENet 送信フラグ |
|---|---|---|
| loss-tolerant | 1 | `ENET_PACKET_FLAG_UNSEQUENCED`(unreliable, unsequenced) |
| must-deliver | 0 | `ENET_PACKET_FLAG_RELIABLE` |

reliable と unreliable を別 channel に分けているのは、同一 channel だと
channel 内 sequence 維持のため reliable 未到着時に unreliable が
HoL blocking されるため(`docs/dev-notes.md` §1.3 参照)。

## 輻輳制御: adaptive packet throttle

ENet は library 既定で `packetThrottle` という確率的スロットルを持つ
(`third_party/enet/peer.c` の `enet_peer_throttle_configure`/
`enet_peer_throttle`、定数は `third_party/enet/include/enet/enet.h` の
`ENET_PEER_PACKET_THROTTLE_*`)。挙動:

- reliable(must-deliver)コマンドの ACK 到達ごとに RTT サンプルを取り、
  直近 `packetThrottleInterval`(既定 5000ms)窓の最小 RTT/最大分散を
  基準値として、サンプル RTT がその基準以下なら throttle を
  `packetThrottleAcceleration`(既定 2)加算、基準+2×分散を超えたら
  `packetThrottleDeceleration`(既定 2)減算する。スケールは
  `ENET_PEER_PACKET_THROTTLE_SCALE`=32(既定値も 32 = 満タン)
- unreliable/unsequenced パケットは送信のたびに `packetThrottleCounter`
  を 7 ずつ(mod 32)進め、`counter > packetThrottle` ならそのパケットを
  **送信キューから破棄**する(`third_party/enet/protocol.c`
  `enet_protocol_send_outgoing_commands` 内、ACK 対象外分岐)。つまり
  `packetThrottle` の値がそのまま unreliable 送信成功率の近似になる
- adjustable。本アダプタは `enet_peer_throttle_configure` を呼ばず
  library 既定のまま計測する(スケール比較の公平性のため v2 では
  transport 既定値からの独自チューニングをしない方針)

**loss-worst regime への影響:** RTT サンプルは reliable(md, 1Hz)の
ACK 到達時にしか取れないため頻度が低く、かつ判定閾値(基準 RTT ±
分散×2)は netem の遅延が固定的でジッタが小さい環境では非常にタイトに
なりうる。バースト loss で reliable 再送が発生すると RTT サンプルが
基準を上振れしやすく、throttle が減速側に振れ続けて unreliable
(loss-tolerant)送信が広範囲にドロップされうる。capacity 崩壊
(br anchor 98→7, vr anchor 90→5、`docs/ledger.md` #11)の一因と見て
`--describe` の `cc_algo` に明記した。将来チューニング版を試す場合は
`enet_peer_throttle_configure(peer, interval, acceleration,
deceleration)` で acceleration を上げる/deceleration を下げる/
interval を広げることで緩められる(upstream 既定の設定 API、
`third_party/enet/peer.c:42-58`)。

## build / test

```sh
cmake -S servers/enet -B build-v2-enet
cmake --build build-v2-enet -j
python3 servers/enet/smoke_test.py build-v2-enet/enet_server build-v2-enet/enet_client
go run ./orchestrator/cmd/orchestrator run -config orchestrator/examples/local-enet.json
```
