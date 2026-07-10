# harness ceiling(raw_udp、wired-v3)— 2026-07-10

バトルの審判基準線。raw_udp(最小 UDP エコー/broadcast、信頼性なし)の
capacity は「このハーネス + wired-v3 regime で出せる上限」であり、以後の
全 transport のセッション結果はこの天井比で読む(spec V1)。

- regime: wired-v3(netem 片道 10ms / loss 0.1%、両方向)
- rig: home-5750ge(`orchestrator/rigs/home.json`)、`scripts/run-sweep.sh` 経由
- config: `orchestrator/examples/sweep-ceiling-wired.json`(warmup 25s、
  duration 10s、seed 1、client_procs 8)
- 34/34 run VALID、censoring なし
- 前提修正: raw_udp server/client 両方の drain budget(ledger #20、
  commit 2f374b1 / 158a889)。修正前は server の window poll 飢餓で
  c80/c48 が measurement_invalid に censored されていた

| workload | ceiling capacity | break 点 | break 原因 |
|---|---|---|---|
| r20p128 | **76** | 77 | staleness_p99 131ms > floor(80ms)+1 interval |
| r20p1000 | **75** | 76 | staleness_p99 131ms > floor(80ms)+1 interval |
| r60p200 | **46** | 47 | staleness_p99 155ms > floor(47ms)+1 interval、client_stall 帰属併記(farm CPU 12% idle) |

読み方の注意:

- broadcast fanout workload なので負荷は conns² で伸びる。capacity 76 ≒
  受信 76×20×76 ≈ 115k msg/s が品質ゲート内で捌ける上限
- r60p200 の break には client_stall 帰属(farm CPU が暇なのに sched p99 が
  budget 超え)が併記されている — 天井の一部が farm 側要因の可能性を
  ゲートが正直に開示している状態。ライブラリ測定でこの近傍が決定的に
  なる場合は farm 増強を検討
- raw 結果: `results-v2/sweep-ceiling-wired/`(git 管理外)
