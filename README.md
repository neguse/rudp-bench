# rudp-bench

Reliable UDP / RUDP / QUIC implementations を同じ workload で比較する benchmark harness。

## v2 移行中

設計: [`docs/superpowers/specs/2026-07-02-rudp-bench-v2-design.md`](docs/superpowers/specs/2026-07-02-rudp-bench-v2-design.md)

v1(`harness/`, `adapters/`, `cmd/rudp-benchctl`, `scripts/`)は**凍結中** — バグ修正含め手を入れない。
新規開発は v2 ディレクトリ(`benchspec/`, `benchkit/`, `servers/`, `orchestrator/`, `calibration/`)のみ。

## Prerequisites

Linux-first. `tc netem`, systemd CPU isolation, vendored native libraries を使う。

```sh
sudo apt-get install -y \
  build-essential cmake git golang-go iproute2 \
  libsodium-dev libnuma-dev libssl-dev libprotobuf-dev protobuf-compiler
```

LiteNetLib は .NET 10 adapter。`litenetlib` を測る場合のみ .NET 10 SDK が必要。

```sh
git submodule update --init --recursive
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Benchmark CLI

全ベンチマーク操作は `rudp-benchctl` に統合。Python 依存なし。

```sh
go build -o rudp-benchctl ./cmd/rudp-benchctl

# canonical sweep (locked scenario — override不可)
./rudp-benchctl run scenarios/canonical.json

# quick smoke test
./rudp-benchctl run scenarios/quick.json

# 単発実行
./rudp-benchctl run --lib coop_rudp --profile echo --conns 50 --duration 5

# 実行計画プレビュー
./rudp-benchctl run scenarios/canonical.json --plan

# dry-run (コマンド出力のみ)
./rudp-benchctl run --lib enet --profile echo --conns 10 --dry-run
```

canonical sweep は `sudo` が必要（`tc netem`, `systemd-run` CPU pinning, cgroup isolation）。

## Docs

- Canonical benchmark: [`docs/CANONICAL.md`](docs/CANONICAL.md)
- Design spec: [`docs/superpowers/specs/2026-04-28-rudp-bench-design.md`](docs/superpowers/specs/2026-04-28-rudp-bench-design.md)
- Development / measurement notes: [`docs/dev-notes.md`](docs/dev-notes.md)
