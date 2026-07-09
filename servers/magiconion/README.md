# MagicOnion v2 transport

Build:

```sh
DOTNET_CLI_HOME=/tmp/dotnet-cli-home dotnet build servers/magiconion/MagicOnionBench.sln
```

Smoke:

```sh
python3 servers/magiconion/smoke_test.py \
  servers/magiconion/MagicOnionBench.Server/bin/Debug/net10.0/MagicOnionBench.Server \
  servers/magiconion/MagicOnionBench.Client/bin/Debug/net10.0/MagicOnionBench.Client
```

The smoke test also checks that the client metrics JSON has the same structural
shape as `benchkit` `bk_metrics_dump_json`: top-level keys, class keys, raw
keys, `log2x16` histogram metadata, and 448-bin histogram arrays.

## Tuned(2026-07-10)

- **ServerStats を Interlocked 化**: StreamingHub のメソッドは複数
  thread-pool スレッドで並行に走るため、単一 lock だと全 hub がメッセージ
  ごとに直列化される(msquic で同型の修正が broadcast の主要律速だった)。
- **Kestrel HTTP/2 flow-control window 拡大**(公式ノブ、--describe に開示):
  InitialConnectionWindowSize 128KB→8MB / InitialStreamWindowSize 96KB→4MB。
  高 conns の受信集約での flow-control 律速を除去。
- 確認(loopback 5s): echo c4 p50 0.37ms、bcast c64 delivery 1.000 /
  p50 23.6ms。
