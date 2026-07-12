# MagicOnion v2 transport

Build:

```sh
DOTNET_CLI_HOME=/tmp/dotnet-cli-home dotnet build -c Release servers/magiconion/MagicOnionBench.sln
```

Smoke:

```sh
python3 servers/magiconion/smoke_test.py \
  servers/magiconion/MagicOnionBench.Server/bin/Release/net10.0/MagicOnionBench.Server \
  servers/magiconion/MagicOnionBench.Client/bin/Release/net10.0/MagicOnionBench.Client
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
- 確認(loopback 5s、Release): echo c4 p50 0.61ms。c64 broadcast は
  同居負荷のある開発機では farm 側 pacing gate(attempted<0.99)で INVALID に
  なるため、スケール挙動の確定は wired rig でのバトル測定に委ねる。

## ramp モード

orchestrator の ramp(単一 run 内の接続数段階増加。契約は
`benchspec/README.md`「ramp mode」)に対応済み(`BenchKit.CS/BenchRamp.cs`)。
`BENCH_RAMP_*` が揃うと phase ごとに接続を追加して per-phase snapshot
(`$BENCH_METRICS_OUT.ramp-*.json`)を書き、最終の cumulative metrics JSON は
書かない(上記 smoke の metrics 形状検査は固定窓経路の話)。stop marker
出現後の connect 失敗は停止手順として graceful に扱う。
