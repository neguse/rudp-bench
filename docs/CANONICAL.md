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

Targets: `raw_udp,mini_rudp,coop_rudp,apex_rudp,enet,kcp,slikenet,raknet,udt4,yojimbo,gns,litenetlib,msquic`

Break rule: each point is N=3. A point is OK when aggregate `valid >= 2/3` and median `delivery_ratio >= 0.95`. The first non-OK connection count is the break point.

Client load generation uses up to 4 client processes. Broadcast profiles split
local connections across those processes but keep the fanout denominator at the
total room size.

## Source Of Truth

- Benchmark execution: [`../scripts/run_canonical_tests.sh`](../scripts/run_canonical_tests.sh)
- Per-run report generation: [`../scripts/render_canonical_report.py`](../scripts/render_canonical_report.py)
- Stable published pointer: [`measurements/current.md`](measurements/current.md)
- Dated measurement reports under `docs/measurements/` are archived run outputs.
- `README.md` and `docs/FINAL_OUTPUT.md` are compatibility pointers only. Do not duplicate result tables there.
