# Canonical Benchmark

このファイルを canonical benchmark の唯一の人間向け入口にする。

## Run

```sh
scripts/run_canonical_tests.sh
```

この repo で "canonical test" と言うときは unit test ではなく、最新の final saturation benchmark 一式を指す。

実行後は `$OUT/report.md` を見る。Markdown 内に `plots/*.png` が埋め込まれ、capacity / delivery / CPU / RTT p95 を確認できる。

## Current Published Result

**open this:** [`measurements/current.md`](measurements/current.md)

`scripts/run_canonical_tests.sh` publishes each run to a dated directory under `docs/measurements/` and updates `docs/measurements/current.md` to point at that latest report. Review and commit both the dated directory and `current.md` when publishing a new current result.

## Canonical Sweep

| profile | workload | mode | traffic | payload | conn sweep |
|---|---|---|---|---:|---|
| `media_relay` | media SFU / relay fanout | broadcast | unreliable 30Hz | 1000B | 1, 5, 50, 75, 100, 125, 150, 200 |
| `game_server` | authoritative game state/event fanout | broadcast | reliable 1Hz + unreliable 20Hz | 128B | 1, 5, 64, 96, 128, 192, 256 |
| `reliable_echo` | reliable transport echo baseline | echo | reliable 50Hz | 64B | 1, 50, 200, 600, 1000, 1500, 2000, 3000 |
| `echo` | mixed 50/50 synthetic baseline | echo | reliable 50Hz + unreliable 50Hz | 64B | 1, 50, 200, 600, 1000, 1500, 2000, 3000 |

Targets: `mini_rudp,coop_rudp,apex_rudp,enet,kcp,slikenet,raknet,udt4,yojimbo,gns,litenetlib,msquic`

`raw_udp` is kept in the repository as the unreliable-only floor baseline. It
is not a normal canonical target for reliable or mixed profiles because it has
no reliable channel; include it only in explicitly-unreliable baseline sweeps.

Break rule: each point is N=3. A point is OK when aggregate `valid >= 2/3` and median `delivery_ratio >= 0.95`. The first non-OK connection count is the break point.

Client load generation: echo profiles use 8 client processes on 8 logical
CPUs (4 physical cores); broadcast profiles use 4 client processes on the
same CPU set. Broadcast profiles split local connections across the
processes but keep the fanout denominator at the total room size.
(2026-06-12: echo raised from 4 processes — at conns>=2000 the 4-process farm
was generation-limited (`client_tick` invalid), understating capable servers.
Broadcast stays at 4: it is not generation-limited there, and 8 processes
measurably degrade receive-side delivery for thread-heavy clients —
gns media_relay c50 drops 0.97→0.52 with 8 processes.)

## CPU Isolation

The canonical layout is role-isolated by physical core:

| role | physical cores | logical CPUs |
|---|---:|---|
| OS / background | 0-2 | 0,1,2,8,9,10 |
| client load generator | 3-6 | 3,4,5,6,11,12,13,14 |
| server under test | 7 | 7,15 |

`scripts/run_canonical_tests.sh` runs `scripts/bench_isolate.sh setup` before
the sweep and tears it down on exit. The benchmark processes are then launched
through `systemd-run` with matching `AllowedCPUs` so OS, client, and server do
not share a physical core.

## Source Of Truth

- Benchmark execution: [`../scripts/run_canonical_tests.sh`](../scripts/run_canonical_tests.sh)
- Per-run report generation: [`../scripts/render_canonical_report.py`](../scripts/render_canonical_report.py)
- Stable published pointer: [`measurements/current.md`](measurements/current.md)
- Dated measurement reports under `docs/measurements/` are archived run outputs.
- `README.md` and `docs/FINAL_OUTPUT.md` are compatibility pointers only. Do not duplicate result tables there.
