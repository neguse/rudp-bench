# Final Output

このプロジェクトの最終アウトプットは、固定 traffic shape で connection count を壊れるまで上げる 3 つの saturation profiles。

詳細レポート: [`measurements/2026-06-03-apex-batch-final/report.md`](measurements/2026-06-03-apex-batch-final/report.md)

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
| `media_relay` | `apex_rudp` | 125 | 150 | packet coalescing 後、旧 strongest の 50 OK / 75 break を超えた。 |
| `game_server` | `apex_rudp` | 128 | 192 | 旧 LiteNetLib 96 OK / 128 break を超えた。 |
| `echo` | `apex_rudp` | 3000 | not broken | final schedule 上限の 3000 conn まで OK。 |

## Data

- capacity table: [`measurements/2026-06-03-apex-batch-final/data/capacity.csv`](measurements/2026-06-03-apex-batch-final/data/capacity.csv)
- all medians: [`measurements/2026-06-03-apex-batch-final/data/summary.csv`](measurements/2026-06-03-apex-batch-final/data/summary.csv)
- run-level results: [`measurements/2026-06-03-apex-batch-final/data/results_all.csv`](measurements/2026-06-03-apex-batch-final/data/results_all.csv)

Older measurement directories are tuning logs, fixed-conn probes, baseline runs, or intermediate reports. Treat this file and the linked apex batch final report as the canonical final output.
