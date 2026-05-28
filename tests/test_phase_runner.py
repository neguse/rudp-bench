#!/usr/bin/env python3
import argparse
import csv
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read_rows(path: Path):
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        results = tmp / "phase_results.csv"
        diagnostics = tmp / "phase_diagnostics.csv"
        scenarios = tmp / "phase_scenarios.csv"
        raw_dir = tmp / "raw"

        subprocess.run(
            [
                str(ROOT / "scripts" / "run_phase1.sh"),
                "--libraries=raw_udp",
                f"--build-dir={args.build_dir}",
                "--rates-r=0",
                "--rates-u=5",
                "--sizes=64",
                "--conns=1",
                "--losses=0",
                "--modes=echo",
                "--duration=1",
                "--warmup=0",
                "--idle=spin",
                "--run-id=phase_runner_test",
                f"--results={results}",
                f"--diagnostics={diagnostics}",
                f"--scenarios={scenarios}",
                f"--raw-dir={raw_dir}",
            ],
            cwd=ROOT,
            check=True,
        )

        result_rows = read_rows(results)
        assert len(result_rows) == 1
        result = result_rows[0]
        assert result["library"] == "raw_udp"
        assert result["valid"] == "1"
        assert result["invalid_reason"] == "ok"
        assert result["delivery_ratio"] == "1.0000"

        scenario_rows = read_rows(scenarios)
        assert len(scenario_rows) == 1
        scenario = scenario_rows[0]
        assert scenario["idle_policy"] == "spin"
        assert scenario["pinning_policy"] == "none"
        assert scenario["supports_reliability"] == "1"
        assert scenario["min_payload_bytes"] == "17"
        assert scenario["max_payload_bytes"] == "65507"
        assert scenario["transport_mode"] == "udp_datagram"

        diagnostic_rows = read_rows(diagnostics)
        assert len(diagnostic_rows) == 2
        by_role = {row["role"]: row for row in diagnostic_rows}
        assert set(by_role) == {"server", "client"}
        assert by_role["client"]["attempted"] == "5"
        assert by_role["client"]["accepted"] == "5"
        assert by_role["client"]["delivered"] == "5"
        assert by_role["client"]["client_tick_ok"] == "1"
        for row in diagnostic_rows:
            assert Path(row["raw_result_path"]).is_file()
            assert Path(row["stdout_path"]).is_file()
            assert Path(row["stderr_path"]).is_file()

        missing_results = tmp / "missing_results.csv"
        missing_diagnostics = tmp / "missing_diagnostics.csv"
        missing_scenarios = tmp / "missing_scenarios.csv"
        missing_raw = tmp / "missing_raw"

        subprocess.run(
            [
                str(ROOT / "scripts" / "run_phase1.sh"),
                "--libraries=litenetlib",
                f"--build-dir={args.build_dir}",
                "--litenetlib-bin=/tmp/rudp-bench-missing-litenetlib",
                "--rates-r=1",
                "--rates-u=0",
                "--sizes=64",
                "--conns=1",
                "--losses=0",
                "--modes=echo",
                "--duration=1",
                "--warmup=0",
                "--idle=spin",
                "--run-id=phase_missing_binary_test",
                f"--results={missing_results}",
                f"--diagnostics={missing_diagnostics}",
                f"--scenarios={missing_scenarios}",
                f"--raw-dir={missing_raw}",
            ],
            cwd=ROOT,
            check=True,
        )

        missing_result = read_rows(missing_results)[0]
        assert missing_result["library"] == "litenetlib"
        assert missing_result["valid"] == "0"
        assert missing_result["invalid_reason"] == "missing_binary"

        missing_by_role = {row["role"]: row for row in read_rows(missing_diagnostics)}
        assert set(missing_by_role) == {"server", "client"}
        for row in missing_by_role.values():
            assert row["exit_reason"] == "missing_binary"
            assert row["exit_status"] == "127"
            assert Path(row["stdout_path"]).is_file()
            stderr_path = Path(row["stderr_path"])
            assert stderr_path.is_file()
            assert "litenetlib binary not found" in stderr_path.read_text()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
