#!/usr/bin/env python3
"""Reduce raw role CSVs into canonical results and diagnostic summaries."""

import argparse
import csv
import os
from pathlib import Path
from typing import Dict, Optional


TIMEOUT_STATUS = 124
MIN_PAYLOAD_BYTES = 16

SUPPORTED_RELIABILITY = {
    "raw_udp": {"u"},
    "udt4": {"r"},
}

# Limits where the current adapter/harness would fail or truncate payloads.
MAX_PAYLOAD_BYTES = {
    "raw_udp": 65507,
    "mini_rudp": 2042,
    "yojimbo": 4096,
}

# These adapters currently multiplex requested logical conns over one real conn.
MAX_CONNS = {
    "slikenet": 1,
    "yojimbo": 1,
}

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
    "exit_status",
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


def int_or_none(value: object) -> Optional[int]:
    try:
        if value is None or value == "":
            return None
        return int(str(value))
    except ValueError:
        return None


def unsupported_reliability(args: argparse.Namespace,
                            server: Optional[Dict[str, str]],
                            client: Optional[Dict[str, str]]) -> bool:
    supported = SUPPORTED_RELIABILITY.get(args.library)
    if supported is not None and args.reliable not in supported:
        return True
    return (server is not None and server.get("reliable") == "na") or (
        client is not None and client.get("reliable") == "na"
    )


def unsupported_payload(args: argparse.Namespace) -> bool:
    size = int_or_none(args.size)
    if size is None:
        return True
    if size < MIN_PAYLOAD_BYTES:
        return True
    max_payload = MAX_PAYLOAD_BYTES.get(args.library)
    return max_payload is not None and size > max_payload


def unsupported_conns(args: argparse.Namespace) -> bool:
    conns = int_or_none(args.conns)
    if conns is None:
        return True
    max_conns = MAX_CONNS.get(args.library)
    return max_conns is not None and conns > max_conns


def role_exit_reason(role: str, raw: Optional[Dict[str, str]], status: str) -> str:
    status_code = int_or_none(status)
    if status_code == TIMEOUT_STATUS:
        return f"{role}_timeout"
    if status_code is not None and status_code != 0:
        return f"{role}_crash"
    if raw is None:
        return "missing_raw_result"
    if raw.get("reliable") == "na":
        return "unsupported_reliability"
    return "ok"


def accepted_count(client: Optional[Dict[str, str]]) -> int:
    if client is None:
        return 0
    accepted = int_or_none(client.get("client_accepted"))
    if accepted is not None:
        return accepted
    return int_or_none(client.get("sent")) or 0


def invalid_reason(server: Optional[Dict[str, str]],
                   client: Optional[Dict[str, str]],
                   args: argparse.Namespace) -> str:
    server_exit = role_exit_reason("server", server, args.server_status)
    client_exit = role_exit_reason("client", client, args.client_status)

    if unsupported_reliability(args, server, client):
        return "unsupported_reliability"
    if unsupported_payload(args):
        return "unsupported_payload"
    if unsupported_conns(args):
        return "unsupported_conns"
    if server_exit == "server_timeout":
        return "server_timeout"
    if client_exit == "client_timeout":
        return "client_timeout"
    if server_exit in ("server_crash", "missing_raw_result"):
        return "server_crash"
    if client_exit in ("client_crash", "missing_raw_result"):
        return "client_crash"
    if client.get("client_tick_ok") == "0":
        return "client_tick"
    if accepted_count(client) == 0:
        return "no_accepted_messages"
    return "ok"


def diagnostic_row(run_id: str, scenario_id: str, role: str,
                   raw: Optional[Dict[str, str]], raw_path: str,
                   status: str, scenario_reason: str) -> Dict[str, object]:
    exit_reason = role_exit_reason(role, raw, status)
    if scenario_reason in (
        "unsupported_reliability",
        "unsupported_payload",
        "unsupported_conns",
    ):
        exit_reason = scenario_reason
    if raw is None:
        return {
            "run_id": run_id,
            "scenario_id": scenario_id,
            "role": role,
            "exit_reason": exit_reason,
            "exit_status": status,
            "raw_result_path": raw_path,
        }
    is_client = role == "client"
    return {
        "run_id": run_id,
        "scenario_id": scenario_id,
        "role": role,
        "exit_reason": exit_reason,
        "exit_status": status,
        "cpu_pct": raw.get("cpu_pct", ""),
        "rss_mb": raw.get("rss_mb", ""),
        "attempted": raw.get("client_offered", "") if is_client else "",
        "accepted": raw.get("client_accepted", raw.get("sent", "")) if is_client else "",
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
    reason = invalid_reason(server, client, args)
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
               diagnostic_row(args.run_id, args.scenario_id, "server", server,
                              args.server, args.server_status, reason))
    append_row(args.diagnostics, DIAGNOSTIC_FIELDS,
               diagnostic_row(args.run_id, args.scenario_id, "client", client,
                              args.client, args.client_status, reason))

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
    append_p.add_argument("--server-status", default="")
    append_p.add_argument("--client-status", default="")
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
