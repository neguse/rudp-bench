# Apex batch final remeasure

**測定日:** 2026-06-03

**位置づけ:** `apex_rudp` の packet coalescing / broadcast fanout 改善後、final saturation profiles と同じ traffic shape で apex だけを N=3 再測定した。

比較対象の他 library は [`../2026-06-02-final-saturation/report.md`](../2026-06-02-final-saturation/report.md) の final run。今回の変更は apex adapter と C++ runner の broadcast helper に閉じており、LiteNetLib adapter は変更していない。

## Change

- server broadcast fanout を `Adapter::send_many` で apex に渡し、apex は unreliable fanout を queue / packet coalescing する。
- apex packet に `FLAG_BATCH` を追加し、複数 logical apex packets を 1 UDP datagram に詰める。
- `send_batch` 共通 path でも同じ coalescing を使い、echo の client->server / server->client も物理 datagram 数を減らす。
- batch 受信時だけ client logical recv budget を入れ、broadcast で受信処理が send schedule を潰さないようにする。

## Capacity

| profile | apex max OK | apex break | old strongest | result |
|---|---:|---:|---|---|
| `media_relay` | 125 | 150 | LiteNetLib / apex 50 OK, 75 break | apex が単独 strongest |
| `game_server` | 128 | 192 | LiteNetLib 96 OK, 128 break | apex が単独 strongest |
| `echo` | 3000 | not broken | LiteNetLib 2000 OK, 3000 break | apex が単独 strongest |

## Apex N=3 medians

| profile | conns | delivery | server CPU | status |
|---|---:|---:|---:|---|
| `media_relay` | 50 | 0.9818 | 21.00% | OK |
| `media_relay` | 75 | 0.9809 | 27.54% | OK |
| `media_relay` | 100 | 0.9790 | 42.99% | OK |
| `media_relay` | 125 | 0.9733 | 59.84% | OK |
| `media_relay` | 150 | 0.6588 | 54.73% | break |
| `game_server` | 64 | 0.9819 | 16.66% | OK |
| `game_server` | 96 | 0.9779 | 21.48% | OK |
| `game_server` | 128 | 0.9717 | 26.71% | OK |
| `game_server` | 192 | 0.7018 | 32.40% | break |
| `echo` | 200 | 0.9902 | 17.63% | OK |
| `echo` | 600 | 0.9902 | 23.13% | OK |
| `echo` | 1000 | 0.9893 | 26.54% | OK |
| `echo` | 1500 | 0.9900 | 32.94% | OK |
| `echo` | 2000 | 0.9905 | 40.67% | OK |
| `echo` | 3000 | 0.9906 | 58.32% | OK |

## Readout

The old apex bottleneck was physical datagram volume, not logical payload handling. `game_server` c96 profile showed server CPU dominated by `sendmmsg`; after coalescing, `game_server` c128 is OK with 26.71% server CPU.

`echo` also improves because client and server can coalesce many per-connection packets to the same UDP endpoint. With `client_procs=4`, apex reaches the full final schedule through c3000.

## Data

- profile definitions: [`data/profiles.csv`](data/profiles.csv)
- capacity: [`data/capacity.csv`](data/capacity.csv)
- medians: [`data/summary.csv`](data/summary.csv)
- run-level results: [`data/results_all.csv`](data/results_all.csv)
- scenarios: [`data/scenarios_all.csv`](data/scenarios_all.csv)
- raw run: `results/final_saturation_apex_batch_20260602T180140Z/` (`results/` is gitignored)
