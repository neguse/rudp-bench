# calibration — 校正スイート

計測器(benchkit / orchestrator)を「答えの分かっている系」で校正してから
未知の transport を測る。項目の定義は
[v2 design spec](../docs/superpowers/specs/2026-07-02-rudp-bench-v2-design.md)
の「校正スイート」節。

| # | 項目 | 実体 | 実行 |
|---|---|---|---|
| 1 | 会計零点(null) | `benchkit/tests/test_metrics_zero.c` | ctest(CI 常設) |
| 2 | 既知故障注入(fault_inject) | `benchkit/tests/test_fault_inject.c` | ctest(CI 常設) |
| 3 | netem 実効値検証(ping/iperf3) | 未実装(netns 実行と同時に整備) | pre-run gate、要 sudo |
| 4 | 必達会計(TCP 系参加者) | 未実装(magiconion 移植後) | canonical 内で兼務 |
| 5 | duration 不変性 | [`duration_invariance.sh`](duration_invariance.sh) | CI 常設(loopback) |

CI は [`.github/workflows/v2.yml`](../.github/workflows/v2.yml)。
sudo が要るのは 3 のみで、それ以外は常設。
