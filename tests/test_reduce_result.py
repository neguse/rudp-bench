#!/usr/bin/env python3
import csv
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REDUCE = ROOT / "scripts" / "reduce_result.py"


RAW_HEADER = (
    "library,encryption,phase,reliable,size,conns,rate,loss,"
    "throughput_mbps,msg_per_sec,rtt_p50_us,rtt_p95_us,rtt_p99_us,"
    "delivered,accepted,delivery_ratio,cpu_pct,rss_mb,connect_ms,duration_s,"
    "mode,idle_policy,flush_policy,client_tick_gap_p99_us,"
    "client_tick_gap_max_us,"
    "client_pacing_lag_p99_us,client_pacing_lag_max_us,"
    "client_missed_pacing,client_attempted,client_accepted,"
    "client_attempted_ratio,client_accepted_ratio,"
    "client_recv_drained_p99,client_recv_drained_max,"
    "client_outstanding_max,client_tick_ok,delivery_dedup_policy\n"
)
RAW_FIELDS = RAW_HEADER.strip().split(",")

BASE_RAW_ROW = {
    "library": "raw_udp",
    "encryption": "off",
    "phase": "1",
    "reliable": "u",
    "size": "64",
    "conns": "1",
    "rate": "100",
    "loss": "0.000",
    "throughput_mbps": "0.051",
    "msg_per_sec": "100",
    "rtt_p50_us": "10",
    "rtt_p95_us": "20",
    "rtt_p99_us": "30",
    "delivered": "200",
    "accepted": "200",
    "delivery_ratio": "1.0000",
    "cpu_pct": "99.00",
    "rss_mb": "12",
    "connect_ms": "0",
    "duration_s": "2",
    "mode": "echo",
    "idle_policy": "spin",
    "flush_policy": "immediate",
    "client_tick_gap_p99_us": "4",
    "client_tick_gap_max_us": "10",
    "client_pacing_lag_p99_us": "3",
    "client_pacing_lag_max_us": "8",
    "client_missed_pacing": "0",
    "client_attempted": "200",
    "client_accepted": "200",
    "client_attempted_ratio": "1.0000",
    "client_accepted_ratio": "1.0000",
    "client_recv_drained_p99": "1",
    "client_recv_drained_max": "1",
    "client_outstanding_max": "1",
    "client_tick_ok": "1",
    "delivery_dedup_policy": "sliding_window_65536_per_conn",
}


def read_rows(path: Path):
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def write_raw(path: Path, **overrides):
    row = BASE_RAW_ROW.copy()
    row.update({k: str(v) for k, v in overrides.items()})
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=RAW_FIELDS)
        writer.writeheader()
        writer.writerow(row)


def run_reduce(args):
    subprocess.run(["python3", str(REDUCE), *args], check=True)


def append_case(
    tmp: Path,
    results: Path,
    diagnostics: Path,
    scenarios: Path,
    scenario_id: str,
    *,
    library: str = "raw_udp",
    reliable: str = "u",
    size: str = "64",
    conns: str = "1",
    server_raw=True,
    client_raw=True,
    server_status: str = "0",
    client_status: str = "0",
    server_overrides=None,
    client_overrides=None,
):
    server = tmp / f"{scenario_id}_server.csv"
    client = tmp / f"{scenario_id}_client.csv"
    common = {
        "library": library,
        "reliable": reliable,
        "size": size,
        "conns": conns,
    }
    if server_raw:
        server_values = common.copy()
        server_values.update(
            {
                "throughput_mbps": "0.000",
                "msg_per_sec": "0",
                "rtt_p50_us": "0",
                "rtt_p95_us": "0",
                "rtt_p99_us": "0",
                "delivered": "0",
                "accepted": "0",
                "delivery_ratio": "0.0000",
                "cpu_pct": "7.50",
                "rss_mb": "11",
                "client_attempted": "0",
                "client_accepted": "0",
                "client_attempted_ratio": "0.0000",
                "client_accepted_ratio": "0.0000",
            }
        )
        server_values.update(server_overrides or {})
        write_raw(
            server,
            **server_values,
        )
    if client_raw:
        client_values = common.copy()
        client_values.update(client_overrides or {})
        write_raw(client, **client_values)

    run_reduce(
        [
            "append",
            "--results",
            str(results),
            "--diagnostics",
            str(diagnostics),
            "--scenarios",
            str(scenarios),
            "--server",
            str(server),
            "--client",
            str(client),
            "--server-status",
            server_status,
            "--client-status",
            client_status,
            "--run-id",
            "test-run",
            "--scenario-id",
            scenario_id,
            "--library",
            library,
            "--reliable",
            reliable,
            "--size",
            size,
            "--conns",
            conns,
            "--rate",
            "100",
            "--loss",
            "0",
            "--mode",
            "echo",
            "--duration",
            "2",
            "--warmup",
            "0",
            "--idle",
            "spin",
        ]
    )


def by_scenario(rows):
    return {r["scenario_id"]: r for r in rows}


def main() -> int:
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        results = tmp / "results.csv"
        diagnostics = tmp / "diagnostics.csv"
        scenarios = tmp / "scenarios.csv"

        run_reduce(
            [
                "init",
                "--results",
                str(results),
                "--diagnostics",
                str(diagnostics),
                "--scenarios",
                str(scenarios),
            ]
        )

        append_case(tmp, results, diagnostics, scenarios, "ok")
        append_case(
            tmp,
            results,
            diagnostics,
            scenarios,
            "unsupported_reliability",
            library="raw_udp",
            reliable="r",
            server_overrides={"reliable": "na"},
            client_overrides={"reliable": "na"},
        )
        append_case(
            tmp,
            results,
            diagnostics,
            scenarios,
            "unsupported_payload",
            library="yojimbo",
            reliable="r",
            size="4097",
            server_raw=False,
            client_raw=False,
        )
        append_case(
            tmp,
            results,
            diagnostics,
            scenarios,
            "mini_payload_3000_valid",
            library="mini_rudp",
            reliable="r",
            size="3000",
        )
        append_case(
            tmp,
            results,
            diagnostics,
            scenarios,
            "msquic_unreliable_oversize",
            library="msquic",
            reliable="u",
            size="1001",
            server_raw=False,
            client_raw=False,
        )
        append_case(
            tmp,
            results,
            diagnostics,
            scenarios,
            "msquic_reliable_1001_valid",
            library="msquic",
            reliable="r",
            size="1001",
        )
        append_case(
            tmp,
            results,
            diagnostics,
            scenarios,
            "unsupported_conns",
            library="slikenet",
            reliable="u",
            conns="2",
            server_raw=False,
            client_raw=False,
        )
        append_case(
            tmp,
            results,
            diagnostics,
            scenarios,
            "yojimbo_conns_2_valid",
            library="yojimbo",
            reliable="r",
            conns="2",
        )
        append_case(
            tmp,
            results,
            diagnostics,
            scenarios,
            "yojimbo_conns_65_unsupported",
            library="yojimbo",
            reliable="r",
            conns="65",
            server_raw=False,
            client_raw=False,
        )
        append_case(
            tmp,
            results,
            diagnostics,
            scenarios,
            "server_timeout",
            server_raw=False,
            server_status="124",
        )
        append_case(
            tmp,
            results,
            diagnostics,
            scenarios,
            "client_crash",
            client_raw=False,
            client_status="2",
        )
        append_case(
            tmp,
            results,
            diagnostics,
            scenarios,
            "client_tick",
            client_overrides={"client_tick_ok": "0"},
        )
        append_case(
            tmp,
            results,
            diagnostics,
            scenarios,
            "no_accepted_messages",
            client_overrides={
                "delivered": "0",
                "accepted": "0",
                "delivery_ratio": "0.0000",
                "client_accepted": "0",
                "client_accepted_ratio": "0.0000",
            },
        )
        append_case(
            tmp,
            results,
            diagnostics,
            scenarios,
            "low_delivery_is_valid",
            client_overrides={
                "delivered": "1",
                "delivery_ratio": "0.0050",
                "rtt_p95_us": "999",
            },
        )
        append_case(
            tmp,
            results,
            diagnostics,
            scenarios,
            "ratio_recomputed",
            client_overrides={
                "delivered": "50",
                "accepted": "100",
                "delivery_ratio": "9.9999",
            },
        )

        canonical = by_scenario(read_rows(results))
        scenario_rows = by_scenario(read_rows(scenarios))
        assert canonical["ok"]["valid"] == "1"
        assert canonical["ok"]["invalid_reason"] == "ok"
        assert canonical["ok"]["delivery_ratio"] == "1.0000"
        assert canonical["ok"]["rtt_p95_us"] == "20"
        assert canonical["ok"]["server_cpu_pct"] == "7.50"

        assert canonical["unsupported_reliability"]["invalid_reason"] == "unsupported_reliability"
        assert canonical["unsupported_payload"]["invalid_reason"] == "unsupported_payload"
        assert canonical["mini_payload_3000_valid"]["valid"] == "1"
        assert canonical["mini_payload_3000_valid"]["invalid_reason"] == "ok"
        assert canonical["msquic_unreliable_oversize"]["invalid_reason"] == "unsupported_payload"
        assert canonical["msquic_reliable_1001_valid"]["valid"] == "1"
        assert canonical["unsupported_conns"]["invalid_reason"] == "unsupported_conns"
        assert canonical["yojimbo_conns_2_valid"]["valid"] == "1"
        assert canonical["yojimbo_conns_65_unsupported"]["invalid_reason"] == "unsupported_conns"
        assert canonical["server_timeout"]["invalid_reason"] == "server_timeout"
        assert canonical["client_crash"]["invalid_reason"] == "client_crash"
        assert canonical["client_tick"]["invalid_reason"] == "client_tick"
        assert canonical["no_accepted_messages"]["invalid_reason"] == "no_accepted_messages"
        assert canonical["low_delivery_is_valid"]["valid"] == "1"
        assert canonical["low_delivery_is_valid"]["invalid_reason"] == "ok"
        assert canonical["ratio_recomputed"]["delivery_ratio"] == "0.5000"
        assert scenario_rows["ok"]["idle_policy"] == "spin"
        assert scenario_rows["ok"]["flush_policy"] == "immediate"
        assert scenario_rows["unsupported_payload"]["flush_policy"] == "poll_send_packets"

        diag = read_rows(diagnostics)
        assert len(diag) == 30
        client_diag = [r for r in diag if r["scenario_id"] == "ok" and r["role"] == "client"][0]
        assert client_diag["attempted"] == "200"
        assert client_diag["accepted"] == "200"
        assert client_diag["accepted_ratio"] == "1.0000"
        assert client_diag["delivery_ratio"] == "1.0000"
        assert client_diag["exit_status"] == "0"
        assert client_diag["client_tick_ok"] == "1"
        assert client_diag["delivery_dedup_policy"] == "sliding_window_65536_per_conn"

        ratio_diag = [
            r for r in diag if r["scenario_id"] == "ratio_recomputed" and r["role"] == "client"
        ][0]
        assert ratio_diag["accepted"] == "100"
        assert ratio_diag["accepted_ratio"] == "0.5000"
        assert ratio_diag["delivery_ratio"] == "0.5000"

        timeout_diag = [
            r for r in diag if r["scenario_id"] == "server_timeout" and r["role"] == "server"
        ][0]
        assert timeout_diag["exit_reason"] == "server_timeout"
        assert timeout_diag["exit_status"] == "124"

        unsupported_payload_diag = [
            r
            for r in diag
            if r["scenario_id"] == "unsupported_payload" and r["role"] == "client"
        ][0]
        assert unsupported_payload_diag["exit_reason"] == "unsupported_payload"

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
