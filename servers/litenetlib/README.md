# LiteNetLib v2 transport(tuned)

LiteNetLib 2.1.4 server+client pair implementing benchspec
(`benchspec/README.md`)。「ライブラリが想定する最速の使い方」をソース読解に
基づいて設計した tune-to-plateau 版(方針: ルート CLAUDE.md)。チューニングは
全て upstream 公式ノブ/API で、`--describe` の `tuning` に開示する。
Structural template: `servers/magiconion/`(solution layout, BenchKit.CS
wiring, client control-channel lifecycle)。ソース引用は tag 2.1.4
(RevenantX/LiteNetLib)。

Build:

```sh
DOTNET_CLI_HOME=/tmp/dotnet-cli-home dotnet build -c Release servers/litenetlib/LiteNetLibBench.sln
```

Smoke:

```sh
python3 servers/litenetlib/smoke_test.py \
  servers/litenetlib/LiteNetLibBench.Server/bin/Release/net10.0/LiteNetLibBench.Server \
  servers/litenetlib/LiteNetLibBench.Client/bin/Release/net10.0/LiteNetLibBench.Client
```

## Thread model

- **Server**: a single `NetManager` in the library's default (non-manual)
  mode — `NetManager.Start()` runs its own receive + logic threads
  internally. One process serves every client, each connecting from a
  distinct remote endpoint, so there's no per-conn socket/thread pressure.
  `--describe` reports this as `internal_worker`.
- **Client**: one `NetManager` per conn, each started with
  `StartInManualMode` (no internal threads at all — `PollEvents()` does the
  receive, `ManualUpdate()` drives retransmit/ping/timeout logic), all pumped
  from the process's single loop. `--describe` reports this as `single`.

  This is *not* a style choice: `LiteNetManager.Connect()` keys peers by
  remote `IPEndPoint` and returns the existing peer (or `null` if a request is
  already pending) when called again for the same remote endpoint —

  ```csharp
  if (TryGetPeer(target, out var peer)) { ... return peer; }
  ```

  Since every conn in a client process targets the *same* server host:port, a
  single `NetManager` can only ever represent one conn to it. Multiple
  `NetManager`s are therefore required, and manual mode is what keeps that
  from turning into the thread explosion the task brief and
  `docs/checklist.md` §1.3/§6 warn about (2 OS threads per non-manual
  `NetManager`). `adapters/litenetlib/Program.cs` (the v1 adapter) hit the
  same constraint and documents the identical per-conn-manager workaround.

Both roles interleave `PollEvents()`/`ManualUpdate()` with the control-channel
`await`s for `ready`→`schedule` (`Shared/LnlPump.cs`) instead of blocking on
them — blocking would stall handshakes/keepalives and either deadlock the run
before `start_at` or silently disconnect peers under `DisconnectTimeout`
(the "deadlock landmine" called out in the task brief; `docs/checklist.md`
§6, §10.1).

## Class mapping

- `loss-tolerant` → `DeliveryMethod.Unreliable` (single UDP datagram, no
  fragmentation — LiteNetLib throws `TooBigPacketException` above
  `NetConstants.MaxUnreliableDataSize`, which is what `--describe`'s
  `max_payload_bytes` reports)
- `must-deliver` → `DeliveryMethod.ReliableOrdered` (fragmenting, effectively
  much larger; capped at the shared `BenchKit.CS.BenchConstants.MaxPayloadBytes`
  ceiling for validation, same as magiconion)

No coalescing (`--describe`: `"coalescing":"none"`) and no library-level
congestion control (`"cc_algo":"none"` — LiteNetLib's reliable channel uses a
fixed window, no adaptive CC). Sends are synchronous, non-blocking library
calls, so — unlike magiconion's gRPC streaming client, which needs an async
send pipe to avoid RTT-bound coalescing — each scheduled slot is sent inline
from the main loop with no queue.

## レイテンシ機構(ソース根拠)と設計

LiteNetLib には遅延源が 2 段ある:

1. **イベント配送**: 既定では受信イベントは `PollEvents()` までキューされる
   (`LiteNetManager.cs:404-413`)→ server 側 poll 粒度(≤15ms)が床になる。
   → server は `UnsyncedReceiveEvent = true` で受信スレッド直発火
   (`LiteNetManager.cs:159,1016-1033`)。
2. **送信 flush**: `Send()` はチャネルに積むだけで、実際のソケット送出は
   logic スレッドの tick(`UpdateTime` 既定 15ms)まで出ない
   (`LiteNetPeer.cs:1187-1213`, `LiteNetManager.cs:498-551`)。
   → 送信後に `TriggerUpdate()`(upstream が送信レイテンシ短縮用と明記、
   `LiteNetManager.cs:110-111,1307`)で logic スレッドを即 wake。

**fanout worker(server)**: broadcast は 1 受信 → 全 peer `Send` の O(N) 仕事
なので、受信スレッド(UnsyncedReceiveEvent)で inline に行うと受信自体が
滞る(c64 で p50 が 622ms に悪化するのを実測)。payload を ArrayPool へ
1 copy して専用 fanout スレッドに委譲し、受信ハンドラは O(1) を保つ。
SendToAll 相当のバッファ共有 API は無く per-peer copy は不可避
(`LiteNetManager.cs:1083-1095`)。

**client 送信**: `CreatePacketFromPool` + 直書き + `SendPooledPacket`
(`LiteNetPeer.cs:349-389`)で per-send の managed alloc/copy 0。
MTU に収まらない md はライブラリの fragment 経路(通常 `Send`)へ
フォールバック。

**client pump**: sleep 上限 1ms(既定構造は 10ms)。manual mode の受信は
pump でしか進まず、sleep 粒度が farm の受信レイテンシ床になる。
spin-pump(sleep なし)は farm proc が core を焼き同居プロセスを starve
させて逆効果だったため不採用。

## チューニング(`--describe` の `tuning` に開示)

| ノブ | 値(既定) | 根拠 |
|---|---|---|
| UnsyncedReceiveEvent(server) | true(false) | 上記 1 |
| UpdateTime(server) | 1ms(15ms)+ TriggerUpdate | 上記 2 |
| MtuDiscovery | true(false) | 既定 off だと MTU 1024 固定(`NetConstants.cs:89-107`)。有効化で 1432 まで交渉、merge 効率と unreliable 上限が上がる |
| UseNativeSockets | true(false) | recvfrom/sendto P/Invoke 直呼びで managed Socket 層と EndPoint alloc を回避(`LiteNetManager.cs:252-255`)。off だと c64 bcast の delivery が悪化するのを実測 |
| DisconnectTimeout | 60s(5s) | 高負荷で pump/相手 ping が滞った際の切断猶予(`LiteNetPeer.cs:1222-1234`) |
| PacketPoolSize | server 16384 / client 4096(1000) | 枯渇すると全 packet が contended pool lock + GC alloc 経路に落ちる(`LiteNetManager.PacketPool.cs:42-80`)。fanout の in-flight は容易に 1000 を超える |

SocketBufferSize(1MB)は定数で公開ノブが無い(`NetConstants.cs:48`)。
farm 側 rcvbuf 増強が必要になった場合(ledger #5 のシグナル発火時)は
ソース vendor か reflection が必要 — 未実施。

## 確認結果(loopback 5s run、Release ビルド、2026-07-10)

- echo c4: VALID delivery 1.000、p50 sched **19.5ms → 1.3ms**(改善は
  Debug 計測でも N=3 で再現)
- broadcast c64(30Hz×1000B、期待 123k msg/s): Debug 計測では delivery
  1.000 / p50 17ms(旧 35ms)だったが、**Release では同居負荷のある開発機の
  判定限界を超えて 0.61〜0.93 に振れる**(farm と server が CPU を取り合う
  loopback 構成の限界)。スケール挙動の確定は wired rig(CPU 分離)での
  バトル測定に委ねる
- 過負荷点では単一 logic スレッドの sendto がライブラリ ceiling
  (crash はしない)。アダプタでは削れない

received packets are recycled explicitly via `reader.Recycle()` after use,
matching the pattern in LiteNetLib's own README sample, rather than setting
`AutoRecycle=true`.
