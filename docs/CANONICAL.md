# Canonical Benchmark

このファイルを canonical benchmark の唯一の人間向け入口にする。

## Run

```sh
scripts/run_canonical_tests.sh
```

この repo で "canonical test" と言うときは unit test ではなく、最新の final saturation benchmark 一式を指す。

実行後は `$OUT/report.md` を見る。Markdown 内に `plots/*.png` が埋め込まれ、capacity / delivery / CPU / RTT p95 を確認できる。

## Current Published Result

**open this:** [`measurements/current/report.md`](measurements/current/report.md)

**capacity data:** [`measurements/current/capacity.csv`](measurements/current/capacity.csv)

`scripts/run_canonical_tests.sh` updates `docs/measurements/current/` after each canonical run. Review and commit that directory when publishing a new current result.

## Canonical Sweep

| profile | workload | mode | traffic | payload | conn sweep |
|---|---|---|---|---:|---|
| `media_relay` | media SFU / relay fanout | broadcast | unreliable 30Hz | 1000B | 50, 75, 100, 125, 150, 200 |
| `game_server` | authoritative game state/event fanout | broadcast | reliable 1Hz + unreliable 20Hz | 128B | 64, 96, 128, 192, 256 |
| `echo` | mixed 50/50 synthetic baseline | echo | reliable 50Hz + unreliable 50Hz | 64B | 200, 600, 1000, 1500, 2000, 3000 |

Targets: `coop_rudp,apex_rudp,litenetlib,enet,gns,raknet`

Break rule: each point is N=3. A point is OK when aggregate `valid >= 2/3` and median `delivery_ratio >= 0.95`. The first non-OK connection count is the break point.

## Source Of Truth

- Benchmark execution: [`../scripts/run_canonical_tests.sh`](../scripts/run_canonical_tests.sh)
- Per-run report generation: [`../scripts/render_canonical_report.py`](../scripts/render_canonical_report.py)
- Stable published result: [`measurements/current/report.md`](measurements/current/report.md)
- Dated measurement reports under `docs/measurements/` are archived run outputs.
- `README.md` and `docs/FINAL_OUTPUT.md` are compatibility pointers only. Do not duplicate result tables there.
