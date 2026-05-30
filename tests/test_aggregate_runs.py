#!/usr/bin/env python3
"""Tests for scripts/aggregate_runs.py (S1: reproducible N-run median + valid
selection)."""

import csv
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
AGG = ROOT / "scripts" / "aggregate_runs.py"

RESULT_FIELDS = [
    "run_id", "scenario_id", "library", "valid", "invalid_reason",
    "delivery_ratio", "rtt_r_p50_us", "rtt_r_p95_us", "rtt_r_p99_us",
    "rtt_u_p50_us", "rtt_u_p95_us", "rtt_u_p99_us", "server_cpu_pct",
]


def write_results(path: Path, rows):
    with path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=RESULT_FIELDS)
        w.writeheader()
        for r in rows:
            w.writerow({k: r.get(k, "") for k in RESULT_FIELDS})


def write_scenarios(path: Path, rows):
    with path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["run_id", "scenario_id", "library", "conns"])
        w.writeheader()
        for r in rows:
            w.writerow(r)


def by_key(rows):
    return {(r["library"], r["scenario_id"]): r for r in rows}


def main() -> int:
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        results = tmp / "results.csv"
        scenarios = tmp / "scenarios.csv"
        out = tmp / "summary.csv"

        # enet/s200: 3 runs, 2 valid (dr 0.99 & 0.991, cpu 60 & 62), 1 invalid.
        # Median over the 2 valid runs => dr 0.9905, cpu 61.
        # kcp/s1000: 3 runs, all invalid (client_tick) => valid=0, note set.
        write_results(results, [
            {"run_id": "r1", "scenario_id": "s200", "library": "enet", "valid": "1",
             "invalid_reason": "ok", "delivery_ratio": "0.990", "server_cpu_pct": "60.0",
             "rtt_u_p99_us": "1000"},
            {"run_id": "r2", "scenario_id": "s200", "library": "enet", "valid": "1",
             "invalid_reason": "ok", "delivery_ratio": "0.991", "server_cpu_pct": "62.0",
             "rtt_u_p99_us": "1100"},
            {"run_id": "r3", "scenario_id": "s200", "library": "enet", "valid": "0",
             "invalid_reason": "client_tick", "delivery_ratio": "0.100", "server_cpu_pct": "99.0",
             "rtt_u_p99_us": "9000"},
            {"run_id": "r1", "scenario_id": "s1000", "library": "kcp", "valid": "0",
             "invalid_reason": "client_tick", "delivery_ratio": "", "server_cpu_pct": ""},
            {"run_id": "r2", "scenario_id": "s1000", "library": "kcp", "valid": "0",
             "invalid_reason": "client_tick", "delivery_ratio": "", "server_cpu_pct": ""},
            {"run_id": "r3", "scenario_id": "s1000", "library": "kcp", "valid": "0",
             "invalid_reason": "client_crash", "delivery_ratio": "", "server_cpu_pct": ""},
        ])
        write_scenarios(scenarios, [
            {"run_id": "r1", "scenario_id": "s200", "library": "enet", "conns": "200"},
            {"run_id": "r1", "scenario_id": "s1000", "library": "kcp", "conns": "1000"},
        ])

        subprocess.run(
            ["python3", str(AGG), "--results", str(results), "--scenarios",
             str(scenarios), "--out", str(out), "--min-valid", "2"],
            check=True,
        )

        with out.open(newline="") as f:
            agg = by_key(list(csv.DictReader(f)))

        enet = agg[("enet", "s200")]
        assert enet["n_total"] == "3", enet
        assert enet["n_valid"] == "2", enet
        assert enet["valid"] == "1", enet
        assert enet["conns"] == "200", enet
        # median of [0.990, 0.991] = 0.9905; invalid 3rd run (0.100) excluded
        assert enet["delivery_ratio_median"] == "0.9905", enet
        assert enet["server_cpu_pct_median"] == "61.00", enet
        assert enet["rtt_u_p99_us_median"] == "1050", enet
        assert set(enet["included_run_ids"].split(";")) == {"r1", "r2"}, enet
        assert enet["note"] == "", enet

        kcp = agg[("kcp", "s1000")]
        assert kcp["n_valid"] == "0", kcp
        assert kcp["valid"] == "0", kcp
        assert kcp["conns"] == "1000", kcp
        assert kcp["delivery_ratio_median"] == "", kcp
        # dominant invalid_reason across the 3 runs is client_tick (2 of 3)
        assert kcp["note"] == "client_tick", kcp

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
