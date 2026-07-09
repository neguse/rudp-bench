# WebSocket v2 transport

ASP.NET Core (Kestrel) server + `ClientWebSocket` client. Raw `System.Net.WebSockets`
via `UseWebSockets`, not SignalR -- one bench message = one binary WebSocket message
(`endOfMessage: true`), framing/fragmentation handled by the stack. No compression
(permessage-deflate is off by default on both sides; never enabled here).

Both traffic classes map to the single reliable ordered WebSocket/TCP stream
(`"loss_tolerant": "reliable-stream"`, `"must_deliver": "reliable-stream"`), matching
the benchspec TCP-family rule. `coalescing` is `"none"`: no sender-side latest-value
replacement is implemented, so this is the simplest honest baseline for a TCP-family
transport (the benchspec permits, but does not require, loss-tolerant coalescing here).

Server-side sends to a given WebSocket are serialized by a per-connection outbound
queue (`System.Threading.Channels.Channel<byte[]>`, one writer task) since only one
`SendAsync` may be in flight per socket at a time. The queue is bounded (4096
messages) with `FullMode=Wait`: backpressure, not drops -- see `--describe`
`tuning` for the disclosed policy. Under packet loss the TCP send buffer backs up
into this queue, which is intentional: that head-of-line delay is the cost this
transport is being measured for, not something to hide behind an app-level drop.

Docs followed: [Microsoft Learn -- WebSockets support in ASP.NET Core](
https://learn.microsoft.com/aspnet/core/fundamentals/websockets).

Build:

```sh
DOTNET_CLI_HOME=/tmp/dotnet-cli-home dotnet build servers/websocket/WebSocketBench.sln
```

Smoke:

```sh
python3 servers/websocket/smoke_test.py \
  servers/websocket/WebSocketBench.Server/bin/Debug/net10.0/WebSocketBench.Server \
  servers/websocket/WebSocketBench.Client/bin/Debug/net10.0/WebSocketBench.Client
```

The smoke test also checks that the client metrics JSON has the same structural
shape as `benchkit` `bk_metrics_dump_json`: top-level keys, class keys, raw
keys, `log2x16` histogram metadata, and 448-bin histogram arrays.

## Tuned(2026-07-10)

- **ServerStats を Interlocked 化**: 受信ループは接続ごとの async タスクで
  並行に走るため、単一 lock だと全接続がメッセージごとに直列化される
  (msquic で同型の修正が broadcast の主要律速だった)。
- **client の受信ループ index キャプチャバグ修正**: `Task.Run(() =>
  ReceiveLoopAsync(ws, (uint)i, ...))` が for 変数 `i` を共有キャプチャして
  おり、Task 起動がループ前進より遅れると複数受信ループが同じ index を読む
  (dedup キー破壊 → 偽 duplicates、c64 broadcast で決定的に再現)。
  ループ内ローカルへの固定で解消。docs/ledger.md #19。
- 確認(loopback 5s): echo c4 p50 0.26ms、bcast c64 delivery 1.000 /
  dup 0 / p50 3.3-5.6ms。
