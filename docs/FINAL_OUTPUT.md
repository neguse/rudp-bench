# 最新 Final Output

**これが最新版 / canonical:** 2026-06-08 UTC / 2026-06-09 JST の RakNet 追加後 full-target final saturation remeasure。

**詳細レポート:** [`measurements/2026-06-08-raknet-final/report.md`](measurements/2026-06-08-raknet-final/report.md)

**capacity table:** [`measurements/2026-06-08-raknet-final/data/capacity.csv`](measurements/2026-06-08-raknet-final/data/capacity.csv)

このプロジェクトの最終アウトプットは、固定 traffic shape で connection count を壊れるまで上げる 3 つの saturation profiles。

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
| `media_relay` | `apex_rudp` | 125 | 150 | RakNet 追加後の full-target remeasure では apex が最大 OK。RakNet は 50 conn で delivery<0.95。 |
| `game_server` | `apex_rudp` | 128 | 192 | RakNet は 64 conn OK / 96 conn client_tick。LiteNetLib は 96 OK、coop/enet は 64 で delivery<0.95。 |
| `echo` | `apex_rudp` | 3000 | not broken | apex のみ schedule 上限 3000 conn まで OK。RakNet は 200 conn で valid_runs=1/3。 |

## Data

- capacity table: [`measurements/2026-06-08-raknet-final/data/capacity.csv`](measurements/2026-06-08-raknet-final/data/capacity.csv)
- all medians: [`measurements/2026-06-08-raknet-final/data/summary.csv`](measurements/2026-06-08-raknet-final/data/summary.csv)
- run-level results: [`measurements/2026-06-08-raknet-final/data/results_all.csv`](measurements/2026-06-08-raknet-final/data/results_all.csv)

このファイルと linked RakNet full remeasure report を canonical final output とする。
