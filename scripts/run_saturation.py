#!/usr/bin/env python3
"""Discover coarse saturation rates through the Phase 1 runner."""

import argparse
import csv
import datetime as dt
import subprocess
import sys
from pathlib import Path
from typing import Dict, Iterable, List, Optional


DEFAULT_LIBS = "mini_rudp,enet,kcp,slikenet,udt4,yojimbo,gns,litenetlib,msquic"
DEFAULT_RATES = "100,1000,10000,100000"

SUMMARY_FIELDS = [
    "run_id",
    "library",
    "reliable",
    "size",
    "conns",
    "rate",
    "loss",
    "mode",
    "idle_policy",
    "flush_policy",
    "valid",
    "invalid_reason",
    "delivery_ratio",
    "accepted_ratio",
    "server_cpu_pct",
    "stop_reason",
    "results_path",
    "diagnostics_path",
    "scenarios_path",
]


def split_csv(value: str) -> List[str]:
    return [item.strip() for item in value.split(",") if item.strip()]


def read_rows(path: Path) -> List[Dict[str, str]]:
    if not path.exists():
        return []
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def read_first_row(path: Path) -> Optional[Dict[str, str]]:
    rows = read_rows(path)
    return rows[0] if rows else None


def read_client_diagnostics(path: Path) -> Optional[Dict[str, str]]:
    for row in read_rows(path):
        if row.get("role") == "client":
            return row
    return None


def to_float(value: object) -> Optional[float]:
    if value is None or value == "":
        return None
    try:
        return float(str(value))
    except ValueError:
        return None


def is_valid(value: object) -> bool:
    return str(value).strip().lower() in ("1", "true", "yes")


def classify_stop_reason(
    result: Optional[Dict[str, str]],
    diagnostics: Optional[Dict[str, str]],
    min_delivery: float,
    min_accepted: float,
    max_server_cpu: float,
) -> str:
    if result is None:
        return "missing_result"
    if not is_valid(result.get("valid")):
        return result.get("invalid_reason") or "invalid"

    delivery_ratio = to_float(result.get("delivery_ratio"))
    if delivery_ratio is None:
        return "missing_delivery_ratio"
    if delivery_ratio < min_delivery:
        return "delivery_ratio"

    accepted_ratio = to_float(diagnostics.get("accepted_ratio") if diagnostics else None)
    if accepted_ratio is None:
        return "missing_accepted_ratio"
    if accepted_ratio < min_accepted:
        return "accepted_ratio"

    server_cpu = to_float(result.get("server_cpu_pct"))
    if server_cpu is not None and server_cpu >= max_server_cpu:
        return "server_cpu"

    return "ok"


def write_header(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as f:
        csv.DictWriter(f, fieldnames=SUMMARY_FIELDS).writeheader()


def append_summary(path: Path, row: Dict[str, object]) -> None:
    with path.open("a", newline="") as f:
        csv.DictWriter(f, fieldnames=SUMMARY_FIELDS).writerow(
            {field: row.get(field, "") for field in SUMMARY_FIELDS}
        )


def require_single_axis(name: str, value: str) -> None:
    if len(split_csv(value)) != 1:
        raise SystemExit(f"--{name} must contain exactly one value for saturation discovery")


def run_phase1(args: argparse.Namespace, lib: str, rate: str, run_dir: Path) -> Dict[str, object]:
    stem = f"{lib}_{args.reliable}_{args.size}_{args.conns}_{rate}_{args.mode}_{args.loss}_{args.idle}"
    results_path = run_dir / f"{stem}_results.csv"
    diagnostics_path = run_dir / f"{stem}_diagnostics.csv"
    scenarios_path = run_dir / f"{stem}_scenarios.csv"
    raw_dir = run_dir / f"{stem}_raw"
    scenario_run_id = f"{args.run_id}_{stem}"

    cmd = [
        args.phase1_script,
        f"--libraries={lib}",
        f"--build-dir={args.build_dir}",
        f"--results={results_path}",
        f"--diagnostics={diagnostics_path}",
        f"--scenarios={scenarios_path}",
        f"--raw-dir={raw_dir}",
        f"--run-id={scenario_run_id}",
        f"--idle={args.idle}",
        f"--reliabilities={args.reliable}",
        f"--sizes={args.size}",
        f"--conns={args.conns}",
        f"--rates={rate}",
        f"--losses={args.loss}",
        f"--modes={args.mode}",
        f"--duration={args.duration}",
        f"--warmup={args.warmup}",
    ]
    if args.loss_injection:
        cmd.append("--loss-injection")

    completed = subprocess.run(cmd, check=False)
    result = read_first_row(results_path)
    diagnostics = read_client_diagnostics(diagnostics_path)
    scenario = read_first_row(scenarios_path)

    if completed.returncode != 0:
        stop_reason = f"runner_exit_{completed.returncode}"
    else:
        stop_reason = classify_stop_reason(
            result,
            diagnostics,
            args.min_delivery,
            args.min_accepted,
            args.max_server_cpu,
        )

    return {
        "run_id": args.run_id,
        "library": lib,
        "reliable": args.reliable,
        "size": args.size,
        "conns": args.conns,
        "rate": rate,
        "loss": args.loss,
        "mode": args.mode,
        "idle_policy": args.idle,
        "flush_policy": scenario.get("flush_policy", "") if scenario else "",
        "valid": result.get("valid", "") if result else "0",
        "invalid_reason": result.get("invalid_reason", "") if result else stop_reason,
        "delivery_ratio": result.get("delivery_ratio", "") if result else "",
        "accepted_ratio": diagnostics.get("accepted_ratio", "") if diagnostics else "",
        "server_cpu_pct": result.get("server_cpu_pct", "") if result else "",
        "stop_reason": stop_reason,
        "results_path": results_path,
        "diagnostics_path": diagnostics_path,
        "scenarios_path": scenarios_path,
    }


def discover(args: argparse.Namespace, libs: Iterable[str], rates: Iterable[str]) -> int:
    run_dir = Path(args.out_dir) / args.run_id
    summary_path = Path(args.summary)
    write_header(summary_path)

    for lib in libs:
        for rate in rates:
            row = run_phase1(args, lib, rate, run_dir)
            append_summary(summary_path, row)
            print(
                f"{lib} rate={rate} delivery={row['delivery_ratio']} "
                f"accepted={row['accepted_ratio']} server_cpu={row['server_cpu_pct']} "
                f"stop={row['stop_reason']}"
            )
            if row["stop_reason"] != "ok":
                break

    print(f"wrote {summary_path}")
    return 0


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--libraries", default=DEFAULT_LIBS)
    p.add_argument("--rates", default=DEFAULT_RATES)
    p.add_argument("--build-dir", default="build")
    p.add_argument("--phase1-script", default="scripts/run_phase1.sh")
    p.add_argument("--out-dir", default="results/saturation")
    p.add_argument("--summary", default="results/saturation.csv")
    p.add_argument("--run-id", default=dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ"))
    p.add_argument("--reliable", default="r")
    p.add_argument("--size", default="64")
    p.add_argument("--conns", default="1")
    p.add_argument("--loss", default="0")
    p.add_argument("--mode", default="echo")
    p.add_argument("--duration", default="10")
    p.add_argument("--warmup", default="2")
    p.add_argument("--idle", default="adaptive", choices=["spin", "adaptive"])
    p.add_argument("--min-delivery", type=float, default=0.95)
    p.add_argument("--min-accepted", type=float, default=0.95)
    p.add_argument("--max-server-cpu", type=float, default=95.0)
    p.add_argument("--loss-injection", action="store_true")
    args = p.parse_args()

    for axis in ("reliable", "size", "conns", "loss", "mode"):
        require_single_axis(axis, getattr(args, axis))

    libs = split_csv(args.libraries)
    rates = split_csv(args.rates)
    if not libs:
        raise SystemExit("--libraries must contain at least one library")
    if not rates:
        raise SystemExit("--rates must contain at least one rate")

    return discover(args, libs, rates)


if __name__ == "__main__":
    sys.exit(main())
