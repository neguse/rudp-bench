# Coop RUDP final

**測定日:** 2026-06-03

**位置づけ:** `coop.md` の要求に合わせた C library 実装 `coop_rudp` を、final saturation profiles と同じ traffic shape で N=3 測定した。

比較対象は [`../2026-06-03-apex-batch-final/report.md`](../2026-06-03-apex-batch-final/report.md) の apex batch final。final 判定は capacity first、同じ max OK では OK point の server CPU を tie-break として扱う。

## Change

- `coop_rudp` C library を追加した。socket vtable、flow limits/stats、queued send、fragment/reassembly、reliable unordered/ordered、unreliable sequenced、SACK-style ACK、retransmit を持つ。
- harness adapter `coop_rudp` を追加した。POSIX UDP backend は `sendmmsg`/`recvmmsg` と physical datagram coalescing を使う。
- adapter hot path では large unreliable payload の harness header だけを copy し、core library API `rudp_recv()` は通常どおり full-copy semantics を保つ。
- echo の ACK processing は reliable outstanding packet seq だけを追跡し、ACK bitmap 受信時の不要な unreliable seq lookup を避ける。
- media/game の pure unreliable packet では adapter config で unreliable-only ACK を省略する。

## Capacity

| profile | coop max OK | coop break | apex max OK | apex break | result |
|---|---:|---:|---:|---:|---|
| `media_relay` | 125 | 150 | 125 | 150 | same capacity, coop lower CPU at OK point |
| `game_server` | 128 | 192 | 128 | 192 | same capacity, coop higher delivery and lower CPU |
| `echo` | 3000 | not broken | 3000 | not broken | same capacity, coop lower CPU |

## Coop N=3 medians

| profile | conns | delivery | server CPU | status |
|---|---:|---:|---:|---|
| `media_relay` | 125 | 0.9682 | 39.54% | OK |
| `media_relay` | 150 | 0.7830 | 52.88% | break |
| `game_server` | 128 | 0.9837 | 23.44% | OK |
| `game_server` | 192 | invalid | n/a | break: client_tick |
| `echo` | 3000 | 0.9901 | 52.58% | OK |

## Apex comparison

| profile | conns | coop delivery | coop CPU | apex delivery | apex CPU | readout |
|---|---:|---:|---:|---:|---:|---|
| `media_relay` | 125 | 0.9682 | 39.54% | 0.9733 | 59.84% | apex delivery is higher, coop CPU is 20.30pt lower and both pass |
| `game_server` | 128 | 0.9837 | 23.44% | 0.9717 | 26.71% | coop wins delivery and CPU |
| `echo` | 3000 | 0.9901 | 52.58% | 0.9906 | 58.32% | delivery is effectively tied, coop CPU is 5.74pt lower |

## Readout

`coop_rudp` reaches the same capacity boundary as apex on all three final profiles. At the OK boundary, it reduces server CPU on all profiles and improves game_server delivery. media_relay remains delivery-valid but has lower delivery than apex; the win there is CPU at the same capacity, not delivery.

## Data

- profile definitions: [`data/profiles.csv`](data/profiles.csv)
- capacity: [`data/capacity.csv`](data/capacity.csv)
- medians: [`data/summary.csv`](data/summary.csv)
- run-level results: [`data/results_all.csv`](data/results_all.csv)
- scenarios: [`data/scenarios_all.csv`](data/scenarios_all.csv)
- raw run: `results/final_boundary_coop_final_n3_20260603T082735Z/` (`results/` is gitignored)
