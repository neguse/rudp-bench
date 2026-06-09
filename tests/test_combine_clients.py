#!/usr/bin/env python3
"""Verify combine_clients percentile algorithm matches harness LatencyHist."""

import argparse
import csv
import importlib.util
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
COMBINE_PATH = ROOT / "scripts" / "combine_clients.py"


def load_combine_module():
    spec = importlib.util.spec_from_file_location("combine_clients", COMBINE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


COMBINE = load_combine_module()

RAW_FIELDS = (
    "library,encryption,phase,rate_r,rate_u,size,conns,loss,"
    "throughput_mbps,msg_per_sec,"
    "rtt_r_p50_us,rtt_r_p95_us,rtt_r_p99_us,"
    "rtt_u_p50_us,rtt_u_p95_us,rtt_u_p99_us,"
    "delivered,accepted,delivered_r,delivered_u,accepted_r,accepted_u,"
    "delivery_ratio,"
    "server_received,server_echo_accepted,"
    "server_received_r,server_received_u,"
    "server_echo_accepted_r,server_echo_accepted_u,"
    "server_recv_drained_p99,server_recv_drained_max,"
    "cpu_pct,rss_mb,connect_ms,duration_s,"
    "mode,idle_policy,flush_policy,client_tick_gap_p99_us,"
    "client_tick_gap_max_us,"
    "client_pacing_lag_p99_us,client_pacing_lag_max_us,"
    "client_missed_pacing,client_attempted,client_accepted,"
    "client_attempted_ratio,client_accepted_ratio,"
    "client_recv_drained_p99,client_recv_drained_max,"
    "client_outstanding_max,client_tick_ok,"
    "conn_peak,conn_disc_transport,conn_disc_peer,"
    "delivery_dedup_policy,"
    "cpu_pct_peak,close_ms"
).split(",")

BASE_ROW = {
    "library": "mini_rudp",
    "encryption": "off",
    "phase": "1",
    "rate_r": "50",
    "rate_u": "0",
    "size": "100",
    "conns": "1",
    "loss": "0.000",
    "throughput_mbps": "0.008",
    "msg_per_sec": "10",
    "rtt_r_p50_us": "0",
    "rtt_r_p95_us": "0",
    "rtt_r_p99_us": "0",
    "rtt_u_p50_us": "0",
    "rtt_u_p95_us": "0",
    "rtt_u_p99_us": "0",
    "delivered": "0",
    "accepted": "0",
    "delivered_r": "0",
    "delivered_u": "0",
    "accepted_r": "0",
    "accepted_u": "0",
    "delivery_ratio": "0.0000",
    "server_received": "0",
    "server_echo_accepted": "0",
    "server_received_r": "0",
    "server_received_u": "0",
    "server_echo_accepted_r": "0",
    "server_echo_accepted_u": "0",
    "server_recv_drained_p99": "0",
    "server_recv_drained_max": "0",
    "cpu_pct": "1.00",
    "cpu_pct_peak": "2.00",
    "rss_mb": "10",
    "connect_ms": "0",
    "close_ms": "1",
    "duration_s": "10",
    "mode": "echo",
    "idle_policy": "spin",
    "flush_policy": "immediate_retransmit_poll",
    "client_tick_gap_p99_us": "0",
    "client_tick_gap_max_us": "0",
    "client_pacing_lag_p99_us": "0",
    "client_pacing_lag_max_us": "0",
    "client_missed_pacing": "0",
    "client_attempted": "0",
    "client_accepted": "0",
    "client_attempted_ratio": "0.0000",
    "client_accepted_ratio": "0.0000",
    "client_recv_drained_p99": "0",
    "client_recv_drained_max": "0",
    "client_outstanding_max": "0",
    "client_tick_ok": "1",
    "conn_peak": "1",
    "conn_disc_transport": "0",
    "conn_disc_peer": "0",
    "delivery_dedup_policy": "sliding_window_65536_per_conn",
}


def csv_value(path: Path, column: str) -> str:
    with path.open(newline="") as f:
        for row in csv.DictReader(f):
            return row[column]
    raise SystemExit(f"{path}: empty CSV")


def csv_row(path: Path):
    with path.open(newline="") as f:
        rows = list(csv.DictReader(f))
    assert len(rows) == 1
    return rows[0]


def write_raw(path: Path, **overrides) -> None:
    row = BASE_ROW.copy()
    row.update({k: str(v) for k, v in overrides.items()})
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=RAW_FIELDS)
        writer.writeheader()
        writer.writerow(row)


def write_empty_bins(path: Path) -> None:
    header = COMBINE.HEADER_STRUCT.pack(
        COMBINE.MAGIC, COMBINE.VERSION, 0, 0, 0, COMBINE.BIN_COUNT
    )
    path.write_bytes(header + b"\0" * (8 * COMBINE.BIN_COUNT))


def run_synthetic_combine(tmp: Path) -> None:
    client_csvs = []
    bins_r = []
    bins_u = []
    for i in range(2):
        client_csv = tmp / f"synthetic_{i}.csv"
        bin_r = tmp / f"synthetic_{i}_r.bin"
        bin_u = tmp / f"synthetic_{i}_u.bin"
        client_csvs.append(client_csv)
        bins_r.append(bin_r)
        bins_u.append(bin_u)
        write_empty_bins(bin_r)
        write_empty_bins(bin_u)

    write_raw(
        client_csvs[0],
        rate_r="10",
        conns="1",
        delivered="35",
        accepted="70",
        delivered_r="30",
        delivered_u="5",
        accepted_r="60",
        accepted_u="10",
        client_attempted="80",
        client_accepted="70",
        client_attempted_ratio="0.8000",
        client_accepted_ratio="0.8750",
    )
    write_raw(
        client_csvs[1],
        rate_r="8",
        conns="1",
        delivered="10",
        accepted="20",
        delivered_r="8",
        delivered_u="2",
        accepted_r="16",
        accepted_u="4",
        client_attempted="40",
        client_accepted="20",
        client_attempted_ratio="0.5000",
        client_accepted_ratio="0.5000",
    )

    combined_csv = tmp / "synthetic_combined.csv"
    subprocess.run(
        [
            "python3",
            str(COMBINE_PATH),
            f"--client-csv={client_csvs[0]}",
            f"--client-csv={client_csvs[1]}",
            f"--bins-r={bins_r[0]}",
            f"--bins-r={bins_r[1]}",
            f"--bins-u={bins_u[0]}",
            f"--bins-u={bins_u[1]}",
            f"--out={combined_csv}",
            "--conns-total=2",
        ],
        check=True,
    )

    combined = csv_row(combined_csv)
    assert combined["client_attempted"] == "120"
    assert combined["client_accepted"] == "90"
    assert combined["delivered"] == "45"
    assert combined["delivered_r"] == "38"
    assert combined["delivered_u"] == "7"
    assert combined["accepted_r"] == "76"
    assert combined["accepted_u"] == "14"
    assert combined["client_attempted_ratio"] == "0.6667"
    assert combined["client_accepted_ratio"] == "0.7500"
    assert combined["delivery_ratio"] == "0.5000"
    assert combined["conn_peak"] == "2"
    assert combined["conn_disc_transport"] == "0"
    assert combined["conn_disc_peer"] == "0"


def run_broadcast_synthetic_combine(tmp: Path) -> None:
    client_csvs = []
    bins_r = []
    bins_u = []
    for i in range(2):
        client_csv = tmp / f"broadcast_{i}.csv"
        bin_r = tmp / f"broadcast_{i}_r.bin"
        bin_u = tmp / f"broadcast_{i}_u.bin"
        client_csvs.append(client_csv)
        bins_r.append(bin_r)
        bins_u.append(bin_u)
        write_empty_bins(bin_r)
        write_empty_bins(bin_u)

    for client_csv in client_csvs:
        write_raw(
            client_csv,
            rate_r="0",
            rate_u="10",
            conns="2",
            duration_s="10",
            mode="broadcast",
            delivered="800",
            accepted="800",
            delivered_u="800",
            accepted_u="800",
            client_attempted="800",
            client_accepted="800",
            client_attempted_ratio="1.0000",
            client_accepted_ratio="1.0000",
        )

    combined_csv = tmp / "broadcast_combined.csv"
    subprocess.run(
        [
            "python3",
            str(COMBINE_PATH),
            f"--client-csv={client_csvs[0]}",
            f"--client-csv={client_csvs[1]}",
            f"--bins-r={bins_r[0]}",
            f"--bins-r={bins_r[1]}",
            f"--bins-u={bins_u[0]}",
            f"--bins-u={bins_u[1]}",
            f"--out={combined_csv}",
            "--conns-total=4",
        ],
        check=True,
    )

    combined = csv_row(combined_csv)
    assert combined["conns"] == "4"
    assert combined["client_attempted"] == "1600"
    assert combined["client_accepted"] == "1600"
    assert combined["client_attempted_ratio"] == "1.0000"
    assert combined["client_accepted_ratio"] == "1.0000"
    assert combined["delivery_ratio"] == "1.0000"


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
        tmp = Path(td)
        run_synthetic_combine(tmp)
        run_broadcast_synthetic_combine(tmp)
        run_smoke(args, tmp)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
