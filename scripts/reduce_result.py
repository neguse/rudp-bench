#!/usr/bin/env python3
"""Reduce raw role CSVs into canonical results and diagnostic summaries."""

import argparse
import csv
import os
from pathlib import Path
from typing import Dict, Optional

import capabilities


TIMEOUT_STATUS = 124
MISSING_BINARY_STATUS = 127

RESULT_FIELDS = [
    "run_id",
    "scenario_id",
    "library",
    "valid",
    "invalid_reason",
    "delivery_ratio",
    "forward_delivery_ratio",
    "forward_delivery_ratio_r",
    "forward_delivery_ratio_u",
    "server_echo_accept_ratio",
    "server_echo_accept_ratio_r",
    "server_echo_accept_ratio_u",
    "return_delivery_ratio",
    "return_delivery_ratio_r",
    "return_delivery_ratio_u",
    "rtt_r_p50_us",
    "rtt_r_p95_us",
    "rtt_r_p99_us",
    "rtt_u_p50_us",
    "rtt_u_p95_us",
    "rtt_u_p99_us",
    "server_cpu_pct",
    "server_cpu_pct_peak",
]

DIAGNOSTIC_FIELDS = [
    "run_id",
    "scenario_id",
    "role",
    "exit_reason",
    "exit_status",
    "cpu_pct",
    "cpu_pct_peak",
    "close_ms",
    "rss_mb",
    "attempted",
    "accepted",
    "delivered",
    "accepted_r",
    "accepted_u",
    "delivered_r",
    "delivered_u",
    "accepted_ratio",
    "delivery_ratio",
    "server_received",
    "server_echo_accepted",
    "server_received_r",
    "server_received_u",
    "server_echo_accepted_r",
    "server_echo_accepted_u",
    "server_recv_drained_p99",
    "server_recv_drained_max",
    "client_tick_ok",
    "client_tick_ok_check",
    "client_tick_gap_p99_us",
    "client_pacing_lag_p99_us",
    "client_recv_drained_p99",
    "client_recv_drained_max",
    "client_outstanding_max",
    "conn_peak",
    "conn_disc_transport",
    "conn_disc_peer",
    "raw_result_path",
    "stdout_path",
    "stderr_path",
    "delivery_dedup_policy",
]

SCENARIO_FIELDS = [
    "run_id",
    "scenario_id",
    "library",
    "rate_r",
    "rate_u",
    "size",
    "conns",
    "loss",
    "mode",
    "duration_s",
    "warmup_s",
    "ramp_up_ms",
    "tail_ms",
    "idle_policy",
    "server_cpu_pin",
    "client_cpu_pin",
    "pinning_policy",
    "flush_policy",
    "supports_reliability",
    "min_payload_bytes",
    "max_payload_bytes",
    "max_connections",
    "transport_mode",
]


def int_or_zero(value: object) -> int:
    try:
        if value is None or value == "":
            return 0
        return int(str(value))
    except ValueError:
        return 0


def primary_channel(args: argparse.Namespace) -> str:
    """Which channel ("r"/"u") drives capability lookups for this scenario.
    Reliable wins when both are active because its policy is usually the more
    interesting one (HoL, retransmits)."""
    if int_or_zero(args.rate_r) > 0:
        return "r"
    return "u"


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


def unsupported_reliable(args: argparse.Namespace) -> bool:
    return int_or_zero(args.rate_r) > 0 and not capabilities.supports_reliability(
        args.library, "r")


def unsupported_unreliable(args: argparse.Namespace) -> bool:
    return int_or_zero(args.rate_u) > 0 and not capabilities.supports_reliability(
        args.library, "u")


def unsupported_payload(args: argparse.Namespace) -> bool:
    size = int_or_none(args.size)
    if size is None:
        return True
    if size < capabilities.MIN_PAYLOAD_BYTES:
        return True
    for channel in ("r", "u"):
        rate = int_or_zero(getattr(args, f"rate_{channel}"))
        if rate <= 0:
            continue
        max_payload = capabilities.max_payload_bytes(args.library, channel)
        if max_payload is not None and size > max_payload:
            return True
    return False


def unsupported_conns(args: argparse.Namespace) -> bool:
    conns = int_or_none(args.conns)
    if conns is None:
        return True
    max_conns = capabilities.max_connections(args.library)
    return max_conns is not None and conns > max_conns


def role_exit_reason(role: str, raw: Optional[Dict[str, str]], status: str) -> str:
    # S4: msquic terminates the process with std::_Exit(0) (harness/main.cc),
    # but ONLY on the success path, AFTER write_output() has produced the raw
    # CSV. So a normal msquic run reaches here as status 0 + present raw = "ok".
    # A real failure during the measured run (a worker SIGSEGV/SIGABRT) kills the
    # process with a non-zero signal *before* _Exit(0) runs, so it still surfaces
    # as {role}_crash here (this is how the v2 report's 1000-conn client_crash
    # was caught). The only thing _Exit(0) masks is a crash during clean teardown
    # — which is intentional and irrelevant to the measurement. If msquic somehow
    # exits 0 without writing a row, raw is None below and we flag missing_raw.
    status_code = int_or_none(status)
    if status_code == TIMEOUT_STATUS:
        return f"{role}_timeout"
    if status_code == MISSING_BINARY_STATUS:
        return f"{role}_missing_binary"
    if status_code is not None and status_code != 0:
        return f"{role}_crash"
    if raw is None:
        return "missing_raw_result"
    return "ok"


def accepted_count(client: Optional[Dict[str, str]]) -> int:
    if client is None:
        return 0
    accepted = int_or_none(client.get("accepted"))
    if accepted is not None:
        return accepted
    accepted = int_or_none(client.get("client_accepted"))
    if accepted is not None:
        return accepted
    return int_or_none(client.get("sent")) or 0


def client_attempted(raw: Dict[str, str]) -> str:
    return raw.get("client_attempted", raw.get("client_offered", ""))


def client_accepted(raw: Dict[str, str]) -> str:
    return raw.get("accepted", raw.get("client_accepted", raw.get("sent", "")))


def client_accepted_ratio(raw: Dict[str, str]) -> str:
    attempted = int_or_none(client_attempted(raw))
    accepted = int_or_none(client_accepted(raw))
    if attempted is None or accepted is None:
        return raw.get("client_accepted_ratio", "")
    if attempted == 0:
        return "0.0000"
    return f"{accepted / attempted:.4f}"


def recomputed_tick_ok(raw: Optional[Dict[str, str]]) -> str:
    """S2: recompute the validity gate from the ratios in the raw CSV instead of
    trusting the producing binary's self-reported client_tick_ok. Both the C++
    harness (runner.cc) and the litenetlib adapter (Program.cs) define tick_ok as
    accepted_ratio>=0.99, plus attempted_ratio>=0.99 when a send rate is set.
    Surfacing this independently computed value lets a reader spot any drift
    between those two implementations (the only reason litenetlib's self-reported
    flag could differ from the C++ libraries')."""
    if raw is None:
        return ""
    try:
        acc_ratio = float(raw.get("client_accepted_ratio") or 0.0)
        att_ratio = float(raw.get("client_attempted_ratio") or 0.0)
    except ValueError:
        return ""
    combined = int_or_zero(raw.get("rate_r")) + int_or_zero(raw.get("rate_u"))
    ok = acc_ratio >= 0.99
    if combined > 0:
        ok = ok and att_ratio >= 0.99
    return "1" if ok else "0"


def canonical_delivery_ratio(client: Optional[Dict[str, str]]) -> str:
    if client is None:
        return ""
    delivered = int_or_none(client.get("delivered"))
    accepted = accepted_count(client)
    if delivered is None:
        return client.get("delivery_ratio", "")
    if accepted == 0:
        return "0.0000"
    return f"{delivered / accepted:.4f}"


def expected_per_send(args: argparse.Namespace) -> int:
    conns = int_or_none(args.conns) or 0
    return conns if args.mode == "broadcast" else 1


def client_outbound_messages(client: Optional[Dict[str, str]],
                             args: argparse.Namespace) -> int:
    accepted = accepted_count(client)
    eps = max(1, expected_per_send(args))
    return accepted // eps if accepted else 0


def planned_channel_outbound_messages(args: argparse.Namespace, channel: str) -> int:
    rate = int_or_zero(getattr(args, f"rate_{channel}"))
    conns = int_or_none(args.conns) or 0
    duration = int_or_none(args.duration) or 0
    return rate * conns * duration


def ratio_str(num: int, den: int) -> str:
    if den <= 0:
        return ""
    return f"{num / den:.4f}"


def forward_delivery_ratio(server: Optional[Dict[str, str]],
                           client: Optional[Dict[str, str]],
                           args: argparse.Namespace) -> str:
    if server is None or client is None:
        return ""
    if "server_received" not in server:
        return ""
    return ratio_str(int_or_zero(server.get("server_received")),
                     client_outbound_messages(client, args))


def server_echo_accept_ratio(server: Optional[Dict[str, str]],
                             args: argparse.Namespace) -> str:
    if server is None:
        return ""
    if "server_received" not in server or "server_echo_accepted" not in server:
        return ""
    received = int_or_zero(server.get("server_received"))
    echo_accepted = int_or_zero(server.get("server_echo_accepted"))
    return ratio_str(echo_accepted, received * max(1, expected_per_send(args)))


def forward_delivery_ratio_channel(server: Optional[Dict[str, str]],
                                   args: argparse.Namespace,
                                   channel: str) -> str:
    if server is None:
        return ""
    col = f"server_received_{channel}"
    if col not in server:
        return ""
    return ratio_str(int_or_zero(server.get(col)),
                     planned_channel_outbound_messages(args, channel))


def server_echo_accept_ratio_channel(server: Optional[Dict[str, str]],
                                     args: argparse.Namespace,
                                     channel: str) -> str:
    if server is None:
        return ""
    received_col = f"server_received_{channel}"
    accepted_col = f"server_echo_accepted_{channel}"
    if received_col not in server or accepted_col not in server:
        return ""
    received = int_or_zero(server.get(received_col))
    echo_accepted = int_or_zero(server.get(accepted_col))
    return ratio_str(echo_accepted, received * max(1, expected_per_send(args)))


def return_delivery_ratio(server: Optional[Dict[str, str]],
                          client: Optional[Dict[str, str]]) -> str:
    if server is None or client is None:
        return ""
    if "server_echo_accepted" not in server:
        return ""
    return ratio_str(int_or_none(client.get("delivered")) or 0,
                     int_or_zero(server.get("server_echo_accepted")))


def return_delivery_ratio_channel(server: Optional[Dict[str, str]],
                                  client: Optional[Dict[str, str]],
                                  channel: str) -> str:
    if server is None or client is None:
        return ""
    server_col = f"server_echo_accepted_{channel}"
    client_col = f"delivered_{channel}"
    if server_col not in server or client_col not in client:
        return ""
    return ratio_str(int_or_zero(client.get(client_col)),
                     int_or_zero(server.get(server_col)))


def scenario_flush_policy(args: argparse.Namespace,
                          server: Optional[Dict[str, str]],
                          client: Optional[Dict[str, str]]) -> str:
    channel = primary_channel(args)
    if not capabilities.supports_reliability(args.library, channel):
        return capabilities.flush_policy(args.library, channel)
    for raw in (client, server):
        if raw is not None and raw.get("flush_policy"):
            return raw["flush_policy"]
    return capabilities.flush_policy(args.library, channel)


def pinning_policy(args: argparse.Namespace) -> str:
    server_pin = args.server_cpu_pin or ""
    client_pin = args.client_cpu_pin or ""
    if not server_pin and not client_pin:
        return "none"
    return f"server={server_pin or 'none'};client={client_pin or 'none'}"


def invalid_reason(server: Optional[Dict[str, str]],
                   client: Optional[Dict[str, str]],
                   args: argparse.Namespace) -> str:
    server_exit = role_exit_reason("server", server, args.server_status)
    client_exit = role_exit_reason("client", client, args.client_status)

    if unsupported_reliable(args):
        return "unsupported_reliable"
    if unsupported_unreliable(args):
        return "unsupported_unreliable"
    if unsupported_payload(args):
        return "unsupported_payload"
    if unsupported_conns(args):
        return "unsupported_conns"
    if server_exit == "server_timeout":
        return "server_timeout"
    if client_exit == "client_timeout":
        return "client_timeout"
    if server_exit == "server_missing_binary" or client_exit == "client_missing_binary":
        return "missing_binary"
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
                   stdout_path: str, stderr_path: str,
                   status: str, scenario_reason: str) -> Dict[str, object]:
    exit_reason = role_exit_reason(role, raw, status)
    if scenario_reason in (
        "unsupported_reliable",
        "unsupported_unreliable",
        "unsupported_payload",
        "unsupported_conns",
        "missing_binary",
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
            "stdout_path": stdout_path,
            "stderr_path": stderr_path,
        }
    is_client = role == "client"
    return {
        "run_id": run_id,
        "scenario_id": scenario_id,
        "role": role,
        "exit_reason": exit_reason,
        "exit_status": status,
        "cpu_pct": raw.get("cpu_pct", ""),
        "cpu_pct_peak": raw.get("cpu_pct_peak", ""),
        "close_ms": raw.get("close_ms", ""),
        "rss_mb": raw.get("rss_mb", ""),
        "attempted": client_attempted(raw) if is_client else "",
        "accepted": client_accepted(raw) if is_client else "",
        "delivered": raw.get("delivered", "") if is_client else "",
        "accepted_r": raw.get("accepted_r", "") if is_client else "",
        "accepted_u": raw.get("accepted_u", "") if is_client else "",
        "delivered_r": raw.get("delivered_r", "") if is_client else "",
        "delivered_u": raw.get("delivered_u", "") if is_client else "",
        "server_received": raw.get("server_received", "") if not is_client else "",
        "server_echo_accepted": raw.get("server_echo_accepted", "") if not is_client else "",
        "server_received_r": raw.get("server_received_r", "") if not is_client else "",
        "server_received_u": raw.get("server_received_u", "") if not is_client else "",
        "server_echo_accepted_r": raw.get("server_echo_accepted_r", "") if not is_client else "",
        "server_echo_accepted_u": raw.get("server_echo_accepted_u", "") if not is_client else "",
        "server_recv_drained_p99": raw.get("server_recv_drained_p99", "") if not is_client else "",
        "server_recv_drained_max": raw.get("server_recv_drained_max", "") if not is_client else "",
        "accepted_ratio": client_accepted_ratio(raw) if is_client else "",
        "delivery_ratio": canonical_delivery_ratio(raw) if is_client else "",
        "client_tick_ok": raw.get("client_tick_ok", "") if is_client else "",
        "client_tick_ok_check": recomputed_tick_ok(raw) if is_client else "",
        "client_tick_gap_p99_us": raw.get("client_tick_gap_p99_us", "") if is_client else "",
        "client_pacing_lag_p99_us": raw.get("client_pacing_lag_p99_us", "") if is_client else "",
        "client_recv_drained_p99": raw.get("client_recv_drained_p99", "") if is_client else "",
        "client_recv_drained_max": raw.get("client_recv_drained_max", "") if is_client else "",
        "client_outstanding_max": raw.get("client_outstanding_max", "") if is_client else "",
        "conn_peak": raw.get("conn_peak", ""),
        "conn_disc_transport": raw.get("conn_disc_transport", ""),
        "conn_disc_peer": raw.get("conn_disc_peer", ""),
        "raw_result_path": raw_path,
        "stdout_path": stdout_path,
        "stderr_path": stderr_path,
        "delivery_dedup_policy": raw.get("delivery_dedup_policy", "") if is_client else "",
    }


def append(args: argparse.Namespace) -> int:
    server = read_raw_row(args.server)
    client = read_raw_row(args.client)
    reason = invalid_reason(server, client, args)
    valid = "1" if reason == "ok" else "0"
    capability_metadata = capabilities.scenario_metadata(args.library,
                                                          primary_channel(args))

    append_row(args.scenarios, SCENARIO_FIELDS, {
        "run_id": args.run_id,
        "scenario_id": args.scenario_id,
        "library": args.library,
        "rate_r": args.rate_r,
        "rate_u": args.rate_u,
        "size": args.size,
        "conns": args.conns,
        "loss": args.loss,
        "mode": args.mode,
        "duration_s": args.duration,
        "warmup_s": args.warmup,
        "ramp_up_ms": args.ramp_up_ms,
        "tail_ms": args.tail_ms,
        "idle_policy": args.idle,
        "server_cpu_pin": args.server_cpu_pin,
        "client_cpu_pin": args.client_cpu_pin,
        "pinning_policy": pinning_policy(args),
        "flush_policy": scenario_flush_policy(args, server, client),
        **capability_metadata,
    })

    append_row(args.diagnostics, DIAGNOSTIC_FIELDS,
               diagnostic_row(args.run_id, args.scenario_id, "server", server,
                              args.server, args.server_stdout, args.server_stderr,
                              args.server_status, reason))
    append_row(args.diagnostics, DIAGNOSTIC_FIELDS,
               diagnostic_row(args.run_id, args.scenario_id, "client", client,
                              args.client, args.client_stdout, args.client_stderr,
                              args.client_status, reason))

    append_row(args.results, RESULT_FIELDS, {
        "run_id": args.run_id,
        "scenario_id": args.scenario_id,
        "library": args.library,
        "valid": valid,
        "invalid_reason": reason,
        "delivery_ratio": canonical_delivery_ratio(client),
        "forward_delivery_ratio": forward_delivery_ratio(server, client, args),
        "forward_delivery_ratio_r": forward_delivery_ratio_channel(server, args, "r"),
        "forward_delivery_ratio_u": forward_delivery_ratio_channel(server, args, "u"),
        "server_echo_accept_ratio": server_echo_accept_ratio(server, args),
        "server_echo_accept_ratio_r": server_echo_accept_ratio_channel(server, args, "r"),
        "server_echo_accept_ratio_u": server_echo_accept_ratio_channel(server, args, "u"),
        "return_delivery_ratio": return_delivery_ratio(server, client),
        "return_delivery_ratio_r": return_delivery_ratio_channel(server, client, "r"),
        "return_delivery_ratio_u": return_delivery_ratio_channel(server, client, "u"),
        "rtt_r_p50_us": client.get("rtt_r_p50_us", "") if client else "",
        "rtt_r_p95_us": client.get("rtt_r_p95_us", "") if client else "",
        "rtt_r_p99_us": client.get("rtt_r_p99_us", "") if client else "",
        "rtt_u_p50_us": client.get("rtt_u_p50_us", "") if client else "",
        "rtt_u_p95_us": client.get("rtt_u_p95_us", "") if client else "",
        "rtt_u_p99_us": client.get("rtt_u_p99_us", "") if client else "",
        "server_cpu_pct": server.get("cpu_pct", "") if server else "",
        "server_cpu_pct_peak": server.get("cpu_pct_peak", "") if server else "",
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
    append_p.add_argument("--server-stdout", default="")
    append_p.add_argument("--server-stderr", default="")
    append_p.add_argument("--client-stdout", default="")
    append_p.add_argument("--client-stderr", default="")
    append_p.add_argument("--server-status", default="")
    append_p.add_argument("--client-status", default="")
    append_p.add_argument("--run-id", required=True)
    append_p.add_argument("--scenario-id", required=True)
    append_p.add_argument("--library", required=True)
    append_p.add_argument("--rate-r", dest="rate_r", required=True)
    append_p.add_argument("--rate-u", dest="rate_u", required=True)
    append_p.add_argument("--size", required=True)
    append_p.add_argument("--conns", required=True)
    append_p.add_argument("--loss", required=True)
    append_p.add_argument("--mode", required=True)
    append_p.add_argument("--duration", required=True)
    append_p.add_argument("--warmup", required=True)
    append_p.add_argument("--ramp-up-ms", dest="ramp_up_ms", default="0")
    append_p.add_argument("--tail-ms", dest="tail_ms", default="500")
    append_p.add_argument("--idle", default="spin", choices=["spin", "adaptive"])
    append_p.add_argument("--server-cpu-pin", default="")
    append_p.add_argument("--client-cpu-pin", default="")
    append_p.set_defaults(func=append)

    args = p.parse_args()
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
