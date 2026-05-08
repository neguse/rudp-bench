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
    "delivered,sent,delivery_ratio,cpu_pct,rss_mb,connect_ms,duration_s,"
    "mode,client_tick_gap_p99_us,client_tick_gap_max_us,"
    "client_pacing_lag_p99_us,client_pacing_lag_max_us,"
    "client_missed_pacing,client_offered,client_accepted,"
    "client_offered_ratio,client_accepted_ratio,"
    "client_recv_drained_p99,client_recv_drained_max,"
    "client_outstanding_max,client_tick_ok\n"
)


def read_rows(path: Path):
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def main() -> int:
    with tempfile.TemporaryDirectory() as td:
        tmp = Path(td)
        server = tmp / "server.csv"
        client = tmp / "client.csv"
        results = tmp / "results.csv"
        diagnostics = tmp / "diagnostics.csv"
        scenarios = tmp / "scenarios.csv"

        server.write_text(
            RAW_HEADER
            + "raw_udp,off,1,u,64,1,100,0.000,0.000,0,0,0,0,"
            + "0,0,0.0000,7.50,11,0,2,echo,0,0,0,0,0,0,0,0.0000,0.0000,0,0,0,0\n"
        )
        client.write_text(
            RAW_HEADER
            + "raw_udp,off,1,u,64,1,100,0.000,0.051,100,10,20,30,"
            + "200,200,1.0000,99.00,12,0,2,echo,4,10,3,8,0,200,200,1.0000,1.0000,1,1,1,1\n"
        )

        subprocess.run(
            [
                "python3",
                str(REDUCE),
                "init",
                "--results",
                str(results),
                "--diagnostics",
                str(diagnostics),
                "--scenarios",
                str(scenarios),
            ],
            check=True,
        )
        subprocess.run(
            [
                "python3",
                str(REDUCE),
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
                "--run-id",
                "test-run",
                "--scenario-id",
                "raw_udp_u_64_1_100_echo_0",
                "--library",
                "raw_udp",
                "--reliable",
                "u",
                "--size",
                "64",
                "--conns",
                "1",
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
            ],
            check=True,
        )

        canonical = read_rows(results)
        assert len(canonical) == 1
        assert canonical[0]["valid"] == "1"
        assert canonical[0]["invalid_reason"] == "ok"
        assert canonical[0]["delivery_ratio"] == "1.0000"
        assert canonical[0]["rtt_p95_us"] == "20"
        assert canonical[0]["server_cpu_pct"] == "7.50"

        diag = read_rows(diagnostics)
        assert len(diag) == 2
        client_diag = [r for r in diag if r["role"] == "client"][0]
        assert client_diag["attempted"] == "200"
        assert client_diag["accepted"] == "200"
        assert client_diag["client_tick_ok"] == "1"

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
