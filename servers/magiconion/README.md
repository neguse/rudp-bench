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
