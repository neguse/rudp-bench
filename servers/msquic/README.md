# msquic — MsQuic v2 server/client(tuned)

benchspec/README.md 準拠の実装対。「ライブラリが想定する最速の使い方」を
ソース読解に基づいて設計した tune-to-plateau 版(方針: ルート CLAUDE.md)。
チューニングは全て upstream 公式ノブで、`--describe` の `tuning` に開示する。

## class mapping

| class | QUIC 機構 |
|---|---|
| loss-tolerant | DATAGRAM(RFC 9221)。`max_datagram_frame_size` と MTU から上限 ~1400B |
| must-deliver | 単一 bidi stream 上の 4B 長框フレーム |

**QUIC 固有の意味論**: datagram は cwnd 逼迫時に**破棄されず送信キューに滞留
する**(`send.h:153-158` — DATAGRAM は BYPASS_CC に含まれない。破棄は MTU
縮小か `CANCEL_ON_BLOCKED` 指定時のみ、`datagram.c:200-231,598-633`)。
つまり過負荷では「落ちる」でなく「遅れる」— 他 transport と損失特性が
根本的に異なる。CC は BBR(開示済み)。

## スレッドモデル(ソース根拠)

- partition(≒CPU コア)ごとに worker スレッド + SO_REUSEPORT ソケット。
  受信は CPU 番号ベースの BPF で partition に steering され、接続は CID に
  partition index を埋めて同一 worker に固定される(`lookup.c:366-370`)。
  **enet/gns と違いライブラリ自体がコア方向にスケールする。**
- 1 接続のイベント callback は単一 worker で直列保証
  (`connection.c:10-18`)。callback を重くするとその worker 上の他接続も
  遅れる(`docs/Execution.md:59-64`)→ アダプタの callback は O(1) を保つ。
- callback 内から他接続への send は公式に安全(対象接続の operation queue
  に積むだけ)— server の broadcast fanout はこの経路。
- UDP 層は recvmmsg / sendmmsg / GSO / GRO を能力検出の上で使用
  (`datapath_epoll.c:1786,2357,2428`)。syscall バッチはライブラリ側で済んでいる。

## 送信経路(copy 削減)

- frame バイト列は refcount 付き `SendBuf` に構築。**broadcast は 1 個の
  SendBuf を全宛先で共有**(per-target の malloc+copy を排除)。
  client は header を SendBuf に直接書く(中間バッファなし)。
- `SendBufferingEnabled=FALSE`: 既定 TRUE は StreamSend データを内部バッファ
  へ都度 heap alloc + copy する(`stream_send.c:487-503`、pool 化されて
  いない)。FALSE でゼロコピーになり、SEND_COMPLETE が ACK 時に遅延する
  (`stream_send.c:1510-1526`)— SendBuf は refcount で寿命管理済みなので適合。
- datagram の送信バッファは SENT(最終状態)まで、stream は SEND_COMPLETE
  まで保持が必要(msquic 契約)→ 完了イベントで unref。
- 受信バッファ(stream/datagram とも)は callback 中のみ有効。echo は
  SendBuf への 1 copy が必須(削れない)。

## 共有状態(callback は複数 worker から並行に走る)

- stats は atomic カウンタ(単一 mutex だと全 worker がメッセージごとに
  直列化され、これが旧実装の broadcast 崩壊の主犯だった)
- conns リストは copy-on-write snapshot(更新は接続イベント時のみ、
  読みは atomic_load)

## チューニング(`--describe` の `tuning` に開示)

| ノブ | 値(既定) | 根拠 |
|---|---|---|
| CongestionControlAlgorithm | BBR(Cubic) | 従来から。高 BDP で伸びる |
| DisconnectTimeoutMs | 60s(16s) | 「最古の未 ACK が 16s 未確認 → QUIC_STATUS_CONNECTION_TIMEOUT で切断」(`loss_detection.c:1853-1866`)が ledger #17 の client crash の機構。延長で過負荷時の切断を猶予 |
| IdleTimeoutMs / KeepAliveIntervalMs | 60s / 5s(30s / off) | 片方向無通信の idle 切断防止(idle は受信でのみリセット、`connection.c:5888-5890`) |
| HandshakeIdleTimeoutMs | 30s(10s) | 接続ストーム時のハンドシェイク猶予 |
| SendBufferingEnabled | FALSE(TRUE) | 上記 |
| StreamRecvWindowDefault | 1MB(64KB) | md fanout の flow control 余裕(auto-tune の起点を引き上げ) |
| DatagramReceiveEnabled | TRUE(FALSE) | lt class の前提 |

未検証の候補(バトル時に A/B): `MaxOperationsPerDrain`(既定 16)引き上げ、
ExecutionProfile MAX_THROUGHPUT、XDP datapath。

## 確認結果(loopback 5s run、2026-07-10)

- echo c4: VALID delivery 1.000、p99 sched 0.93ms
- broadcast c64(30Hz×1000B、期待 123k msg/s): delivery **0.34 → 1.000**、
  p50 sched 1.44s → 20ms(atomic stats + COW conns + 共有 SendBuf)
- broadcast c128: VALID・全 proc exit 0(crash なし)。delivery 0.008 =
  datagram が CC キューに滞留する QUIC の意味論どおりの正直な過負荷挙動
  (farm 側 ACK 遅延で BBR が絞られる寄与も含む — wired + farm 増強で再測)

## build / test

```sh
cmake -S servers/msquic -B build-v2-msquic -DCMAKE_BUILD_TYPE=Release
cmake --build build-v2-msquic -j
python3 servers/msquic/smoke_test.py build-v2-msquic/msquic_server build-v2-msquic/msquic_client
go run ./orchestrator/cmd/orchestrator run -config orchestrator/examples/local-msquic.json
```
