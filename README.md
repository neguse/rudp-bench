# rudp-bench

Reliable UDP / RUDP / QUIC implementations を同じ workload で比較する benchmark harness。

## Final Output

まずここを見る:

[`docs/FINAL_OUTPUT.md`](docs/FINAL_OUTPUT.md)

最終評価は、固定 traffic shape で connection count を壊れるまで上げる 3 profiles。

| profile | workload | strongest result |
|---|---|---|
| `media_relay` | media SFU / relay fanout | `litenetlib` / `apex_rudp`: 50 OK, 75 break |
| `game_server` | authoritative game state/event fanout | `litenetlib`: 96 OK, 128 break |
| `echo` | mixed 50/50 synthetic baseline | `litenetlib`: 2000 OK, 3000 break |

古い `docs/measurements/*` は tuning log / intermediate report。最終結論は final saturation を見る。

## Build

```sh
git submodule update --init --recursive
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Re-run Final Benchmark

最終 benchmark を再実行する:

```sh
scripts/run_final_saturation_profiles.py --out results/final_saturation_profiles
```

Raw outputs are written under `results/`. Published final data lives in `docs/measurements/2026-06-02-final-saturation/data/`.

## Docs

- Final output summary: [`docs/FINAL_OUTPUT.md`](docs/FINAL_OUTPUT.md)
- Full final report: [`docs/measurements/2026-06-02-final-saturation/report.md`](docs/measurements/2026-06-02-final-saturation/report.md)
- Capacity table: [`docs/measurements/2026-06-02-final-saturation/data/capacity.csv`](docs/measurements/2026-06-02-final-saturation/data/capacity.csv)
- Design spec: [`docs/superpowers/specs/2026-04-28-rudp-bench-design.md`](docs/superpowers/specs/2026-04-28-rudp-bench-design.md)
- Development / measurement notes: [`docs/dev-notes.md`](docs/dev-notes.md)
