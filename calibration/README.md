# calibration

計測器(benchkit と orchestrator)が正しく数えているかを、答えの分かっている形で確かめる。

| 項目 | 実体 |
|---|---|
| 会計の零点 | `benchkit/tests/test_metrics_zero.c` |
| authoritative-state の会計 | `benchkit/tests/test_authoritative_metrics.c` |
| 故障の注入 | `benchkit/tests/test_fault_inject.c` |
| 計測窓の長さで到達率が変わらないこと | [`duration_invariance.sh`](duration_invariance.sh)(loopback の raw_udp) |

どれも root 権限なしで動き、CI で毎回回る。
