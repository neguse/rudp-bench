# Final Output

このプロジェクトの最終アウトプットは、固定 traffic shape で connection count を壊れるまで上げる 3 つの saturation profiles。

詳細レポート: [`measurements/2026-06-02-final-saturation/report.md`](measurements/2026-06-02-final-saturation/report.md)

## Profiles

| profile | 位置づけ | mode | traffic | payload | conn sweep |
|---|---|---|---|---:|---|
| `media_relay` | media SFU / relay | broadcast | unreliable 30Hz | 1000B | 50, 75, 100, 125, 150, 200 |
| `game_server` | authoritative game server | broadcast | reliable 1Hz + unreliable 20Hz | 128B | 64, 96, 128, 192, 256 |
| `echo` | synthetic mixed baseline | echo | reliable 50Hz + unreliable 50Hz | 64B | 200, 600, 1000, 1500, 2000, 3000 |

## Break Rule

各 point は N=3。`valid >= 2/3` かつ median `delivery_ratio >= 0.95` なら OK。最初に OK でなくなった conn を break point とする。

## Result Summary

| profile | winner / strongest | max OK | break | notes |
|---|---|---:|---:|---|
| `media_relay` | `litenetlib` / `apex_rudp` | 50 | 75 | 50 conn は両者 delivery 0.98 前後。75 conn で両者 break。 |
| `game_server` | `litenetlib` | 96 | 128 | apex / GNS は 64 OK、96 で break。ENet は 64 で threshold 未満。 |
| `echo` | `litenetlib` | 2000 | 3000 | apex は 1000 OK、1500 で break。ENet / GNS は 600 OK、1000 で break。 |

## Data

- capacity table: [`measurements/2026-06-02-final-saturation/data/capacity.csv`](measurements/2026-06-02-final-saturation/data/capacity.csv)
- all medians: [`measurements/2026-06-02-final-saturation/data/summary.csv`](measurements/2026-06-02-final-saturation/data/summary.csv)
- run-level results: [`measurements/2026-06-02-final-saturation/data/results_all.csv`](measurements/2026-06-02-final-saturation/data/results_all.csv)

Older measurement directories are tuning logs, fixed-conn probes, or intermediate reports. Treat this file and the linked final saturation report as the canonical final output.
