# rudp-bench

Reliable UDP / RUDP / QUIC implementations を同じ workload で比較する benchmark harness。

## Prerequisites

This project is Linux-first. The benchmark path uses loopback `tc netem`,
systemd CPU isolation, and several vendored native libraries.

Install the baseline toolchain:

```sh
sudo apt-get install -y \
  build-essential cmake git python3 python3-pip iproute2 \
  libsodium-dev libnuma-dev libssl-dev libprotobuf-dev protobuf-compiler
python3 -m pip install -r scripts/requirements.txt
```

LiteNetLib is built as a separate .NET adapter and currently targets
`.NET 10` (`net10.0`). Install a .NET 10 SDK if you want the `litenetlib`
target and smoke test.

Initialize dependencies and build:

```sh
git submodule update --init --recursive
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

The full canonical benchmark requires `sudo` for `tc netem` and `systemd-run`
CPU pinning. It is intentionally not part of the normal unit-test loop.

## Canonical Benchmark

**Start here:** [`docs/CANONICAL.md`](docs/CANONICAL.md)

`docs/CANONICAL.md` is the single human-facing entrypoint for the canonical sweep definition, current published result, and generated report workflow. Do not duplicate result tables in this README.

## Docs

- Canonical benchmark: [`docs/CANONICAL.md`](docs/CANONICAL.md)
- Design spec: [`docs/superpowers/specs/2026-04-28-rudp-bench-design.md`](docs/superpowers/specs/2026-04-28-rudp-bench-design.md)
- Development / measurement notes: [`docs/dev-notes.md`](docs/dev-notes.md)
