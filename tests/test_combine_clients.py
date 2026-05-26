#!/usr/bin/env python3
"""Verify combine_clients percentile algorithm matches harness LatencyHist."""

import argparse
import csv
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def csv_value(path: Path, column: str) -> str:
    with path.open(newline="") as f:
        for row in csv.DictReader(f):
            return row[column]
    raise SystemExit(f"{path}: empty CSV")


def run_smoke(args: argparse.Namespace, tmp: Path) -> None:
    """Run a single client + server, write bin sidecars, then merge through
    combine_clients with N=1 and confirm the percentiles agree."""
    harness = Path(args.build_dir) / "harness" / "rudp-bench"
    assert harness.exists(), f"harness binary missing: {harness}"

    port = "39101"
    server_csv = tmp / "srv.csv"
    client_csv = tmp / "cli.csv"
    bins_r = tmp / "rtt_r.bin"
    bins_u = tmp / "rtt_u.bin"
    combined_csv = tmp / "combined.csv"

    server = subprocess.Popen(
        [
            str(harness),
            "--library=mini_rudp",
            "--role=server",
            f"--port={port}",
            "--rate-r=50",
            "--rate-u=50",
            "--size=64",
            "--conns=5",
            "--duration=2",
            "--warmup=0",
            "--idle=spin",
            f"--out={server_csv}",
        ]
    )
    try:
        # Brief settle so the server is bound before the client connects.
        subprocess.run(["sleep", "0.3"], check=False)
        subprocess.run(
            [
                str(harness),
                "--library=mini_rudp",
                "--role=client",
                "--host=127.0.0.1",
                f"--port={port}",
                "--rate-r=50",
                "--rate-u=50",
                "--size=64",
                "--conns=5",
                "--duration=2",
                "--warmup=0",
                "--idle=spin",
                f"--out={client_csv}",
                f"--bins-r-out={bins_r}",
                f"--bins-u-out={bins_u}",
            ],
            check=True,
        )
    finally:
        server.wait(timeout=10)

    subprocess.run(
        [
            "python3",
            str(ROOT / "scripts" / "combine_clients.py"),
            f"--client-csv={client_csv}",
            f"--bins-r={bins_r}",
            f"--bins-u={bins_u}",
            f"--out={combined_csv}",
            "--conns-total=5",
        ],
        check=True,
    )

    for column in (
        "rtt_r_p50_us",
        "rtt_r_p95_us",
        "rtt_r_p99_us",
        "rtt_u_p50_us",
        "rtt_u_p95_us",
        "rtt_u_p99_us",
    ):
        harness_value = csv_value(client_csv, column)
        combined_value = csv_value(combined_csv, column)
        assert harness_value == combined_value, (
            f"{column}: harness={harness_value!r} != combine_clients={combined_value!r}"
        )


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--build-dir", default="build")
    args = p.parse_args()
    with tempfile.TemporaryDirectory() as td:
        run_smoke(args, Path(td))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
