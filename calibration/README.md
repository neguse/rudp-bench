# calibration — 校正スイート

計測器(benchkit / orchestrator)を「答えの分かっている系」で校正してから
未知の solution treatment を測る。公開測定での位置付けは
[ADR-0002](../docs/adr/0002-benchmark-methodology.md)、wire/metricsの真値は
[benchspec](../benchspec/README.md)に従う。

| # | 項目 | 実体 | 実行 |
|---|---|---|---|
| 0 | rig preflight | `orchestrator doctor` | campaign前後、qdisc確認に権限が必要 |
| 1 | 会計零点 | `benchkit/tests/test_metrics_zero.c` | ctest(CI常設) |
| 2 | authoritative会計 | `benchkit/tests/test_authoritative_metrics.c` | ctest(CI常設) |
| 3 | 既知故障注入 | `benchkit/tests/test_fault_inject.c` | ctest(CI常設) |
| 4 | netem実効値 | `orchestrator/netops/gate.go` | netem付きrunのpre-run gate、要sudo |
| 5 | duration不変性 | [`duration_invariance.sh`](duration_invariance.sh) | CI常設(loopback)、`CALIBRATION_DIR`指定時は生runを保存 |
| 6 | scenario conformance | raw UDPの3 scenario smoke | CI常設(loopback) |

CI は [`.github/workflows/v2.yml`](../.github/workflows/v2.yml)。Doctorのqdisc検査と
netem実効値以外はroot権限なしで常設する。校正FAILの結果はtransportの性能FAILへ
読み替えず、campaign/blockを`INVALID`にする。
