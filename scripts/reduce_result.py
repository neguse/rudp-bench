#!/usr/bin/env python3
"""Reduce raw role CSVs into canonical results and diagnostic summaries."""

import argparse
import csv
import os
from pathlib import Path
from typing import Dict, Optional


RESULT_FIELDS = [
    "run_id",
    "scenario_id",
    "library",
    "valid",
    "invalid_reason",
    "delivery_ratio",
    "rtt_p50_us",
    "rtt_p95_us",
    "rtt_p99_us",
    "server_cpu_pct",
]

DIAGNOSTIC_FIELDS = [
    "run_id",
    "scenario_id",
    "role",
    "exit_reason",
    "cpu_pct",
    "rss_mb",
    "attempted",
    "accepted",
    "delivered",
    "accepted_ratio",
    "delivery_ratio",
    "client_tick_ok",
    "client_tick_gap_p99_us",
    "client_pacing_lag_p99_us",
    "raw_result_path",
]

SCENARIO_FIELDS = [
    "run_id",
    "scenario_id",
    "library",
    "reliable",
    "size",
    "conns",
    "rate",
    "loss",
    "mode",
    "duration_s",
    "warmup_s",
]


def read_raw_row(path: str) -> Optional[Dict[str, str]]:
    if not path or not os.path.exists(path):
        return None
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        return None
    return rows[0]


def ensure_header(path: str, fields) -> None:
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    if p.exists() and p.stat().st_size > 0:
        return
    with p.open("w", newline="") as f:
        csv.DictWriter(f, fieldnames=fields).writeheader()


def append_row(path: str, fields, row: Dict[str, object]) -> None:
    ensure_header(path, fields)
    with open(path, "a", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writerow({k: row.get(k, "") for k in fields})


def invalid_reason(server: Optional[Dict[str, str]],
                   client: Optional[Dict[str, str]]) -> str:
    if client is None:
        return "client_timeout"
    if server is None:
        return "server_timeout"
    if client.get("reliable") == "na":
        return "unsupported_reliability"
    if client.get("client_tick_ok") == "0":
        return "client_tick"
    try:
        accepted = int(client.get("sent", "0"))
    except ValueError:
        accepted = 0
    if accepted == 0:
        return "no_accepted_messages"
    return "ok"


def diagnostic_row(run_id: str, scenario_id: str, role: str,
                   raw: Optional[Dict[str, str]], raw_path: str) -> Dict[str, object]:
    if raw is None:
        return {
            "run_id": run_id,
            "scenario_id": scenario_id,
            "role": role,
            "exit_reason": "missing_raw_result",
            "raw_result_path": raw_path,
        }
    is_client = role == "client"
    return {
        "run_id": run_id,
        "scenario_id": scenario_id,
        "role": role,
        "exit_reason": "ok",
        "cpu_pct": raw.get("cpu_pct", ""),
        "rss_mb": raw.get("rss_mb", ""),
        "attempted": raw.get("client_offered", "") if is_client else "",
        "accepted": raw.get("sent", "") if is_client else "",
        "delivered": raw.get("delivered", "") if is_client else "",
        "accepted_ratio": raw.get("client_accepted_ratio", "") if is_client else "",
        "delivery_ratio": raw.get("delivery_ratio", "") if is_client else "",
        "client_tick_ok": raw.get("client_tick_ok", "") if is_client else "",
        "client_tick_gap_p99_us": raw.get("client_tick_gap_p99_us", "") if is_client else "",
        "client_pacing_lag_p99_us": raw.get("client_pacing_lag_p99_us", "") if is_client else "",
        "raw_result_path": raw_path,
    }


def append(args: argparse.Namespace) -> int:
    server = read_raw_row(args.server)
    client = read_raw_row(args.client)
    reason = invalid_reason(server, client)
    valid = "1" if reason == "ok" else "0"

    append_row(args.scenarios, SCENARIO_FIELDS, {
        "run_id": args.run_id,
        "scenario_id": args.scenario_id,
        "library": args.library,
        "reliable": args.reliable,
        "size": args.size,
        "conns": args.conns,
        "rate": args.rate,
        "loss": args.loss,
        "mode": args.mode,
        "duration_s": args.duration,
        "warmup_s": args.warmup,
    })

    append_row(args.diagnostics, DIAGNOSTIC_FIELDS,
               diagnostic_row(args.run_id, args.scenario_id, "server", server, args.server))
    append_row(args.diagnostics, DIAGNOSTIC_FIELDS,
               diagnostic_row(args.run_id, args.scenario_id, "client", client, args.client))

    append_row(args.results, RESULT_FIELDS, {
        "run_id": args.run_id,
        "scenario_id": args.scenario_id,
        "library": args.library,
        "valid": valid,
        "invalid_reason": reason,
        "delivery_ratio": client.get("delivery_ratio", "") if client else "",
        "rtt_p50_us": client.get("rtt_p50_us", "") if client else "",
        "rtt_p95_us": client.get("rtt_p95_us", "") if client else "",
        "rtt_p99_us": client.get("rtt_p99_us", "") if client else "",
        "server_cpu_pct": server.get("cpu_pct", "") if server else "",
    })
    return 0


def init(args: argparse.Namespace) -> int:
    for path, fields in [
        (args.results, RESULT_FIELDS),
        (args.diagnostics, DIAGNOSTIC_FIELDS),
        (args.scenarios, SCENARIO_FIELDS),
    ]:
        p = Path(path)
        if p.exists():
            p.unlink()
        ensure_header(path, fields)
    return 0


def main() -> int:
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)

    init_p = sub.add_parser("init")
    init_p.add_argument("--results", required=True)
    init_p.add_argument("--diagnostics", required=True)
    init_p.add_argument("--scenarios", required=True)
    init_p.set_defaults(func=init)

    append_p = sub.add_parser("append")
    append_p.add_argument("--results", required=True)
    append_p.add_argument("--diagnostics", required=True)
    append_p.add_argument("--scenarios", required=True)
    append_p.add_argument("--server", required=True)
    append_p.add_argument("--client", required=True)
    append_p.add_argument("--run-id", required=True)
    append_p.add_argument("--scenario-id", required=True)
    append_p.add_argument("--library", required=True)
    append_p.add_argument("--reliable", required=True)
    append_p.add_argument("--size", required=True)
    append_p.add_argument("--conns", required=True)
    append_p.add_argument("--rate", required=True)
    append_p.add_argument("--loss", required=True)
    append_p.add_argument("--mode", required=True)
    append_p.add_argument("--duration", required=True)
    append_p.add_argument("--warmup", required=True)
    append_p.set_defaults(func=append)

    args = p.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
