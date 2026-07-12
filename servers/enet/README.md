# enet — ENet server/client(tuned)

benchspec/README.md 準拠の実装対。「ライブラリが想定する最速の使い方」を
ソース読解に基づいて設計した tune-to-plateau 版(方針: ルート CLAUDE.md)。
チューニングは全て upstream 公式 API で、`--describe` の `tuning` に開示する。

## class mapping

| class | channel | ENet 送信フラグ |
|---|---|---|
| loss-tolerant | 1 | `ENET_PACKET_FLAG_UNSEQUENCED`(unreliable, unsequenced) |
| must-deliver | 0 | `ENET_PACKET_FLAG_RELIABLE` |

reliable と unreliable を別 channel に分けているのは、同一 channel だと
channel 内 sequence 維持のため reliable 未到着時に unreliable が
HoL blocking されるため(`docs/log/dev-notes.md` §1.3 参照)。

**制約**: unsequenced packet は 1 fragment 長
(≈ MTU 1392 − ヘッダ ≈ 1350B)を超えると reliable fragment に格上げされる
(`third_party/enet/peer.c:136-146`)。loss-tolerant の意味論が保てるのは
その payload 長まで。

## イベントループ設計

`enet_host_service` は 1 呼び出しで最大 1 event しか返さず、毎回
全 peer 走査の送信パス×2 + 受信 + timeout 判定を回す
(`third_party/enet/protocol.c:1830,1846,1862`)。event ごとに `service(0)` を
呼ぶと多 peer で O(events×peers) になるため、dispatch 済みイベントは
`enet_host_check_events`(I/O なし、`protocol.c:1768-1778`)で引き切り、
空になったら `service(0)` で次の I/O パスを回す。

1 回のループの budget はイベント数ではなく仕事量で bound する:
`4096 / fanout(接続数)`、下限 64(`server.c` の service budget)。
broadcast はイベント 1 件の仕事が O(接続数)なので、固定イベント数だと
高 conns で 1 呼び出しが budget×conns 送信に膨らみ、benchkit 制御チャネル
(schedule/window poll)が飢える(raw_udp server と同じ negative window
margin、`docs/ledger.md` #20 と同族)。

## 送信経路(copy 削減)

- ENet の送信はペイロード copy が `enet_packet_create` 時の 1 回だけで、
  以降は iovec 参照の zero-copy(`packet.c:33-41`, `protocol.c:1573`,
  `unix.c:463-466`)。
- **server**: 受信 packet をそのまま `enet_peer_send` で転送する(re-create
  しない)。受信経路で class 相当の flag が packet に付与済みのため
  (`protocol.c:462,506`)class mapping は保存される。broadcast は 1 packet を
  全 peer で refcount 共有(`enet_host_broadcast` と同方式、
  `host.c:271-288`)し、per-peer の malloc+copy を発生させない。
- **client**: `enet_packet_create(NULL, len, flags)` で無初期化 packet を
  確保し、header を packet バッファへ直接書く(中間バッファの memcpy を
  削減)。header 32B 以降は受信側で読まれない契約。

## チューニング(`--describe` の `tuning` に開示)

| 項目 | 値 | 根拠 |
|---|---|---|
| `enet_peer_throttle_configure` | accel=32, decel=0 | 既定(accel=2,decel=2)は burst loss で throttle が 32→0 に滑落し、unreliable の ~97% をライブラリが送信キューで自己破棄する(`docs/ledger.md` #11)。throttle は reliable の実効 window(`packetThrottle×windowSize/32`、`protocol.c:1470`)にも掛かる |
| `enet_peer_timeout` | min=10s, max=60s(既定 5s/30s) | 高負荷で service が間引かれると reliable timeout / ping(reliable、`peer.c:453`)経由で切断され client が落ちる(`docs/ledger.md` #8)。死活検出の遅延と引き換えに延長 |
| SO_RCVBUF / SO_SNDBUF | 4MB(既定 256KB、`enet.h:213-214`) | broadcast fanout の burst で kernel バッファが溢れる |

### packet throttle の機構(参考)

reliable ACK ごとに RTT サンプルを取り、基準 RTT±分散との比較で
`packetThrottle` を accel 加算 / decel 減算する(`peer.c:61-90`)。
unreliable は送信のたびに counter を進め `counter > packetThrottle` なら
送信キューから破棄(`protocol.c:1522-1554`)。decel=0 は減速を無効化し
throttle を満タン(32/32)に保つ = 適応的破棄をしない。loss 環境での
輻輳応答はライブラリ既定より攻撃的になる。これは開示済みの設計判断。

## スケール特性(ソース根拠)

- 送信パスは全 peer 走査(`protocol.c:1611-1613`)。service 1 回あたり×2。
- 帯域 throttle は帯域無制限(host_create 引数 0,0)なら O(N²) 経路
  (`host.c:376-419`)に入らない。
- 受信は 1 service あたり最大 256 datagram(`protocol.c:1236`)、
  sendmmsg/recvmmsg 相当のバッチ syscall は無い(datagram ごとに
  sendmsg/recvmsg、`protocol.c:1727`, `unix.c:500`)。ここがライブラリ自体の
  syscall 律速点で、アダプタ側では削れない。

## ramp モード

orchestrator の ramp(単一 run 内の接続数段階増加。契約は
`benchspec/README.md`「ramp mode」)に対応済み(`../ramp.h` を使用)。
`BENCH_RAMP_*` が揃うと phase ごとに接続を追加して per-phase snapshot
(`$BENCH_METRICS_OUT.ramp-*.json`)を書き、最終の cumulative metrics は
書かない。ramp 有効時は `enet_host_create` の peerCount が `total_conns` になる。

## build / test

```sh
cmake -S servers/enet -B build-v2-enet
cmake --build build-v2-enet -j
python3 servers/enet/smoke_test.py build-v2-enet/enet_server build-v2-enet/enet_client
go run ./orchestrator/cmd/orchestrator run -config orchestrator/examples/local-enet.json
go run ./orchestrator/cmd/orchestrator run -config orchestrator/examples/bcast-enet.json
```
