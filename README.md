# rudp-bench

Reliable UDP / RUDP / QUIC implementations を同じ workload で比較する benchmark harness。

## 最新結果 / Canonical Final Result

**これが最新版:** [`docs/FINAL_OUTPUT.md`](docs/FINAL_OUTPUT.md)

**最新測定:** [`docs/measurements/2026-06-08-raknet-final/report.md`](docs/measurements/2026-06-08-raknet-final/report.md)

**最新データ:** [`docs/measurements/2026-06-08-raknet-final/data/capacity.csv`](docs/measurements/2026-06-08-raknet-final/data/capacity.csv)

2026-06-08 UTC / 2026-06-09 JST の RakNet 追加後 full-target remeasure を、この repo の canonical final result とする。

| profile | workload | strongest result |
|---|---|---|
| `media_relay` | media SFU / relay fanout | `apex_rudp`: 125 OK, 150 break |
| `game_server` | authoritative game state/event fanout | `apex_rudp`: 128 OK, 192 break |
| `echo` | mixed 50/50 synthetic baseline | `apex_rudp`: 3000 OK, not broken |

## Canonical Benchmark Test

```sh
scripts/run_canonical_tests.sh
```

この repo で「canonical test」と言うときは、unit test ではなく最新の final saturation benchmark 一式を指す。手元でも再測定でも、基本は個別の benchmark script 直打ちではなくこの入口を使う。

この script は build 後に、`coop_rudp,apex_rudp,litenetlib,enet,gns,raknet` で `media_relay` / `game_server` / `echo` の canonical profiles を N=3 で全実行する。

実行後は `$OUT/report.md` を見る。Markdown 内に `plots/*.png` が埋め込まれ、capacity / delivery / CPU / RTT p95 をそのまま確認できる。

## Re-run Final Benchmark

最終 benchmark を再実行する:

```sh
scripts/run_canonical_tests.sh --out results/final_saturation_profiles
```

Raw outputs are written under `results/`. Published final data lives in `docs/measurements/2026-06-08-raknet-final/data/`.
The generated report lives at `results/final_saturation_profiles/report.md` when using the command above.

## Docs

- Final output summary: [`docs/FINAL_OUTPUT.md`](docs/FINAL_OUTPUT.md)
- Current measurement pointer: [`docs/measurements/CURRENT.md`](docs/measurements/CURRENT.md)
- Measurements index: [`docs/measurements/README.md`](docs/measurements/README.md)
- Full final report: [`docs/measurements/2026-06-08-raknet-final/report.md`](docs/measurements/2026-06-08-raknet-final/report.md)
- Capacity table: [`docs/measurements/2026-06-08-raknet-final/data/capacity.csv`](docs/measurements/2026-06-08-raknet-final/data/capacity.csv)
- Design spec: [`docs/superpowers/specs/2026-04-28-rudp-bench-design.md`](docs/superpowers/specs/2026-04-28-rudp-bench-design.md)
- Development / measurement notes: [`docs/dev-notes.md`](docs/dev-notes.md)
