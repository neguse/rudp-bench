# LiteNetLib v2 transport

Idiomatic LiteNetLib 2.1.4 server+client pair implementing benchspec v1
(`benchspec/README.md`). Structural template: `servers/magiconion/` (solution
layout, BenchKit.CS wiring, client control-channel lifecycle). LiteNetLib
usage patterns verified against the library's own README sample
(`EventBasedNetListener` + `NetManager` + explicit `PollEvents()` loop,
`peer.Send(data, DeliveryMethod)`) and, where the README didn't say enough,
against LiteNetLib 2.1.4 source (`LiteNetManager.cs`, `NetPeer.cs`,
`NetConstants.cs`) and reflection over the built NuGet DLL.

Build:

```sh
DOTNET_CLI_HOME=/tmp/dotnet-cli-home dotnet build servers/litenetlib/LiteNetLibBench.sln
```

Smoke:

```sh
python3 servers/litenetlib/smoke_test.py \
  servers/litenetlib/LiteNetLibBench.Server/bin/Debug/net10.0/LiteNetLibBench.Server \
  servers/litenetlib/LiteNetLibBench.Client/bin/Debug/net10.0/LiteNetLibBench.Client
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

## Library settings

Every `NetManager` property is left at its LiteNetLib default (verified by
reflecting a freshly-constructed `NetManager` against 2.1.4: `AutoRecycle` /
`UnsyncedEvents` = `false`, `IPv6Enabled` = `true`, `UpdateTime` = `15`,
`UseNativeSockets` = `false`). `--describe`'s `tuning` array is therefore
empty — no v1 overrides (e.g. `PacketPoolSize`) were carried into v2, per the
v2 rule that undisclosed defaults must not silently change. Received packets
are recycled explicitly via `reader.Recycle()` after use, matching the
pattern in LiteNetLib's own README sample, rather than setting
`AutoRecycle=true`.
