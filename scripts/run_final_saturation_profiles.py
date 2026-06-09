#!/usr/bin/env python3
"""Run final profile saturation sweeps by increasing connection count.

The final benchmark profiles keep traffic shape fixed and raise `conns` until a
library breaks. A point is considered OK when the aggregate over repeated runs is
valid and median delivery is at least --min-delivery.
"""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))
import capabilities

DEFAULT_LIBS = (
    "raw_udp,mini_rudp,coop_rudp,apex_rudp,enet,kcp,slikenet,raknet,"
    "udt4,yojimbo,gns,litenetlib,msquic"
)
DEFAULT_RUNS = "1 2 3"
DEFAULT_NETEM_ARGS = "25 5 1 100000"
DEFAULT_MEDIA_CONNS = "1 5 50 75 100 125 150 200"
DEFAULT_GAME_CONNS = "1 5 64 96 128 192 256"
DEFAULT_ECHO_CONNS = "1 50 200 600 1000 1500 2000 3000"
DEFAULT_RELIABLE_ECHO_CONNS = "1 50 200 600 1000 1500 2000 3000"


@dataclass(frozen=True)
class Profile:
    name: str
    use_case: str
    mode: str
    rate_r: int
    rate_u: int
    size: int
    conns: List[int]
    client_procs: int
    notes: str


PROFILE_FIELDS = [
    "profile",
    "use_case",
    "mode",
    "rate_r",
    "rate_u",
    "size",
    "conns_schedule",
    "client_procs",
    "notes",
]

CAPACITY_FIELDS = [
    "profile",
    "library",
    "status",
    "last_ok_conns",
    "last_ok_delivery",
    "last_ok_server_cpu",
    "break_conns",
    "break_reason",
    "break_delivery",
    "break_server_cpu",
]


def split_csv(value: str) -> List[str]:
    return [item.strip() for item in value.split(",") if item.strip()]


def split_runs(value: str) -> List[str]:
    return [item.strip() for item in value.replace(",", " ").split() if item.strip()]


def split_ints(value: str) -> List[int]:
    out: List[int] = []
    for item in value.replace(",", " ").split():
        item = item.strip()
        if not item:
            continue
        out.append(int(item))
    if not out:
        raise ValueError("connection schedule must not be empty")
    return out


def read_rows(path: Path) -> List[Dict[str, str]]:
    if not path.exists():
        return []
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def to_float(value: object) -> Optional[float]:
    if value is None or value == "":
        return None
    try:
        return float(str(value))
    except ValueError:
        return None


def combine_csv(paths: Iterable[Path], out: Path) -> int:
    fieldnames: Optional[List[str]] = None
    rows: List[Dict[str, str]] = []
    for path in sorted(paths):
        if not path.exists():
            continue
        with path.open(newline="") as f:
            reader = csv.DictReader(f)
            if reader.fieldnames is None:
                continue
            if fieldnames is None:
                fieldnames = list(reader.fieldnames)
            for row in reader:
                rows.append(row)
    if fieldnames is None:
        return 0
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames, lineterminator="\n")
        writer.writeheader()
        writer.writerows({k: r.get(k, "") for k in fieldnames} for r in rows)
    return len(rows)


def aggregate(results: Path, scenarios: Path, out: Path, min_valid: int) -> None:
    subprocess.run(
        [
            "python3",
            "scripts/aggregate_runs.py",
            "--results",
            str(results),
            "--scenarios",
            str(scenarios),
            "--out",
            str(out),
            "--min-valid",
            str(min_valid),
        ],
        check=True,
    )


def profile_defs(args: argparse.Namespace) -> List[Profile]:
    return [
        Profile(
            name="media_relay",
            use_case="media_sfu_unreliable_fanout",
            mode="broadcast",
            rate_r=0,
            rate_u=30,
            size=1000,
            conns=split_ints(args.media_conns),
            client_procs=1,
            notes="near-MTU media packets, full-room unreliable fanout",
        ),
        Profile(
            name="game_server",
            use_case="authoritative_game_snapshot_event_fanout",
            mode="broadcast",
            rate_r=1,
            rate_u=20,
            size=128,
            conns=split_ints(args.game_conns),
            client_procs=1,
            notes="20Hz state/input fanout plus 1Hz reliable gameplay events",
        ),
        Profile(
            name="reliable_echo",
            use_case="reliable_transport_echo_baseline",
            mode="echo",
            rate_r=50,
            rate_u=0,
            size=64,
            conns=split_ints(args.reliable_echo_conns),
            client_procs=args.echo_client_procs,
            notes="reliable-only echo baseline for stream/reliable transports",
        ),
        Profile(
            name="echo",
            use_case="synthetic_mixed_echo_baseline",
            mode="echo",
            rate_r=50,
            rate_u=50,
            size=64,
            conns=split_ints(args.echo_conns),
            client_procs=args.echo_client_procs,
            notes="mixed 50/50 echo baseline used for implementation validation",
        ),
    ]


def write_profiles(path: Path, profiles: List[Profile]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=PROFILE_FIELDS, lineterminator="\n")
        writer.writeheader()
        for p in profiles:
            writer.writerow(
                {
                    "profile": p.name,
                    "use_case": p.use_case,
                    "mode": p.mode,
                    "rate_r": p.rate_r,
                    "rate_u": p.rate_u,
                    "size": p.size,
                    "conns_schedule": " ".join(str(c) for c in p.conns),
                    "client_procs": p.client_procs,
                    "notes": p.notes,
                }
            )


def run_phase(args: argparse.Namespace, profile: Profile, conns: int,
              run: str, libs: List[str], out: Path) -> int:
    run_id = f"{profile.name}_c{conns}_r{run}"
    log_path = out / f"log_{run_id}.txt"
    client_procs = max(1, min(profile.client_procs, conns))
    cmd = [
        args.phase1_script,
        f"--libraries={','.join(libs)}",
        f"--build-dir={args.build_dir}",
        f"--mode={profile.mode}",
        f"--rate-r={profile.rate_r}",
        f"--rate-u={profile.rate_u}",
        f"--size={profile.size}",
        f"--conns={conns}",
        f"--duration={args.duration}",
        f"--tail-ms={args.tail_ms}",
        f"--idle={args.idle}",
        f"--client-procs={client_procs}",
        f"--isolate={args.isolate}",
        f"--server-cpu={args.server_cpu}",
        f"--client-cpu={args.client_cpu}",
        f"--litenetlib-bin={args.litenetlib_bin}",
        f"--results={out / f'res_{run_id}.csv'}",
        f"--diagnostics={out / f'diag_{run_id}.csv'}",
        f"--scenarios={out / f'scen_{run_id}.csv'}",
        f"--raw-dir={out / f'raw_{run_id}'}",
        f"--run-id={run_id}",
    ]
    print(
        f"[{dt.datetime.now().strftime('%H:%M:%S')}] "
        f"profile={profile.name} conns={conns} run={run} libs={','.join(libs)} START",
        flush=True,
    )
    with log_path.open("w") as log:
        completed = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, check=False)
    print(
        f"[{dt.datetime.now().strftime('%H:%M:%S')}] "
        f"profile={profile.name} conns={conns} run={run} exit={completed.returncode} DONE",
        flush=True,
    )
    return completed.returncode


def conn_summary(out: Path, profile: str, conns: int, min_valid: int) -> Path:
    result_paths = list(out.glob(f"res_{profile}_c{conns}_r*.csv"))
    scenario_paths = list(out.glob(f"scen_{profile}_c{conns}_r*.csv"))
    results = out / f"results_{profile}_c{conns}.csv"
    scenarios = out / f"scenarios_{profile}_c{conns}.csv"
    summary = out / f"summary_{profile}_c{conns}.csv"
    combine_csv(result_paths, results)
    combine_csv(scenario_paths, scenarios)
    aggregate(results, scenarios, summary, min_valid)
    return summary


def stop_reason(row: Optional[Dict[str, str]], min_delivery: float) -> str:
    if row is None:
        return "missing_summary"
    if row.get("valid") != "1":
        note = row.get("note") or f"valid_runs={row.get('n_valid', '')}/{row.get('n_total', '')}"
        if note.startswith("unsupported_"):
            return note
        return f"aggregate_invalid:{note}"
    delivery = to_float(row.get("delivery_ratio_median"))
    if delivery is None:
        return "missing_delivery"
    if delivery < min_delivery:
        return f"delivery<{min_delivery:.2f}"
    return "ok"


def unsupported_profile_reason(profile: Profile, lib: str) -> str:
    for channel, rate in (("r", profile.rate_r), ("u", profile.rate_u)):
        if rate <= 0:
            continue
        if not capabilities.supports_reliability(lib, channel):
            return "unsupported_reliable" if channel == "r" else "unsupported_unreliable"
        max_payload = capabilities.max_payload_bytes(lib, channel)
        if max_payload is not None and profile.size > max_payload:
            return "unsupported_payload"
    return ""


def unsupported_conns(lib: str, conns: int) -> bool:
    max_conns = capabilities.max_connections(lib)
    return max_conns is not None and conns > max_conns


def ensure_capacity_row(capacity: Dict[tuple, Dict[str, str]],
                        profile: Profile, lib: str) -> Dict[str, str]:
    return capacity.setdefault(
        (profile.name, lib),
        {
            "profile": profile.name,
            "library": lib,
            "status": "not_started",
            "last_ok_conns": "",
            "last_ok_delivery": "",
            "last_ok_server_cpu": "",
            "break_conns": "",
            "break_reason": "",
            "break_delivery": "",
            "break_server_cpu": "",
        },
    )


def mark_capacity_stop(capacity: Dict[tuple, Dict[str, str]], profile: Profile,
                       lib: str, conns: int, reason: str) -> None:
    cap = ensure_capacity_row(capacity, profile, lib)
    if reason.startswith("unsupported_") and not cap["last_ok_conns"]:
        cap["status"] = "unsupported"
        cap["last_ok_conns"] = "unsupported"
    else:
        cap["status"] = "broken"
    cap["break_conns"] = str(conns)
    cap["break_reason"] = reason


def mark_first_measured_failure(cap: Dict[str, str], row: Dict[str, str],
                                conns: int, reason: str) -> None:
    cap["status"] = "below_gate" if reason.startswith("delivery<") else "failed_gate"
    cap["last_ok_conns"] = cap["status"]
    cap["break_conns"] = str(conns)
    cap["break_reason"] = reason
    cap["break_delivery"] = row.get("delivery_ratio_median", "")
    cap["break_server_cpu"] = row.get("server_cpu_pct_median", "")


def update_capacity_rows(capacity: Dict[tuple, Dict[str, str]], profile: Profile,
                         libs: List[str], rows: Dict[str, Dict[str, str]],
                         conns: int, min_delivery: float) -> List[str]:
    broken: List[str] = []
    for lib in libs:
        row = rows.get(lib)
        reason = stop_reason(row, min_delivery)
        cap = ensure_capacity_row(capacity, profile, lib)
        if reason == "ok":
            cap["status"] = "not_broken"
            cap["last_ok_conns"] = str(conns)
            cap["last_ok_delivery"] = row.get("delivery_ratio_median", "") if row else ""
            cap["last_ok_server_cpu"] = row.get("server_cpu_pct_median", "") if row else ""
            continue
        if reason.startswith("unsupported_") and not cap["last_ok_conns"]:
            mark_capacity_stop(capacity, profile, lib, conns, reason)
            broken.append(lib)
            continue
        if not cap["last_ok_conns"] and row is not None:
            mark_first_measured_failure(cap, row, conns, reason)
            broken.append(lib)
            continue
        cap["status"] = "broken"
        cap["break_conns"] = str(conns)
        cap["break_reason"] = reason
        cap["break_delivery"] = row.get("delivery_ratio_median", "") if row else ""
        cap["break_server_cpu"] = row.get("server_cpu_pct_median", "") if row else ""
        broken.append(lib)
    return broken


def write_capacity(path: Path, capacity: Dict[tuple, Dict[str, str]]) -> None:
    with path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=CAPACITY_FIELDS, lineterminator="\n")
        writer.writeheader()
        for key in sorted(capacity):
            writer.writerow({field: capacity[key].get(field, "") for field in CAPACITY_FIELDS})


def combine_all(out: Path, min_valid: int) -> None:
    combine_csv(out.glob("res_*_c*_r*.csv"), out / "results_all.csv")
    combine_csv(out.glob("scen_*_c*_r*.csv"), out / "scenarios_all.csv")
    aggregate(out / "results_all.csv", out / "scenarios_all.csv", out / "summary.csv", min_valid)


def apply_netem(args: argparse.Namespace, out: Path) -> None:
    if not args.netem:
        return
    cmd = ["sudo", "scripts/netem.sh", "apply"] + split_runs(args.netem_args)
    with (out / "netem_apply.txt").open("w") as f:
        subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, check=True)


def clear_netem(args: argparse.Namespace, out: Path) -> None:
    if not args.netem:
        return
    with (out / "netem_clear.txt").open("w") as f:
        subprocess.run(["sudo", "scripts/netem.sh", "clear"], stdout=f, stderr=subprocess.STDOUT, check=False)


def run(args: argparse.Namespace) -> int:
    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    libs = split_csv(args.libraries)
    runs = split_runs(args.runs)
    profiles = profile_defs(args)
    write_profiles(out / "profiles.csv", profiles)
    capacity: Dict[tuple, Dict[str, str]] = {}

    try:
        apply_netem(args, out)
        for profile in profiles:
            active: List[str] = []
            for lib in libs:
                reason = unsupported_profile_reason(profile, lib)
                if reason:
                    mark_capacity_stop(capacity, profile, lib, profile.conns[0], reason)
                else:
                    active.append(lib)
            for conns in profile.conns:
                if not active:
                    break
                unsupported_now = [lib for lib in active if unsupported_conns(lib, conns)]
                for lib in unsupported_now:
                    mark_capacity_stop(capacity, profile, lib, conns, "unsupported_conns")
                if unsupported_now:
                    active = [lib for lib in active if lib not in set(unsupported_now)]
                    print(
                        f"  profile={profile.name} unsupported_conns={','.join(unsupported_now)} "
                        f"remaining={','.join(active) or '-'}",
                        flush=True,
                    )
                if not active:
                    break
                for r in runs:
                    rc = run_phase(args, profile, conns, r, active, out)
                    if rc != 0:
                        print(
                            f"runner returned {rc} for profile={profile.name} conns={conns} run={r}",
                            file=sys.stderr,
                        )
                summary_path = conn_summary(out, profile.name, conns, args.min_valid)
                summary_rows = {
                    row.get("library", ""): row for row in read_rows(summary_path)
                }
                broken = update_capacity_rows(capacity, profile, active, summary_rows, conns, args.min_delivery)
                for row in read_rows(summary_path):
                    if row.get("library") in active:
                        reason = stop_reason(row, args.min_delivery)
                        print(
                            f"  {profile.name} {row.get('library')} c={conns} "
                            f"delivery={row.get('delivery_ratio_median')} "
                            f"cpu={row.get('server_cpu_pct_median')} stop={reason}",
                            flush=True,
                        )
                if broken:
                    active = [lib for lib in active if lib not in set(broken)]
                    print(
                        f"  profile={profile.name} broken={','.join(broken)} remaining={','.join(active) or '-'}",
                        flush=True,
                    )
        combine_all(out, args.min_valid)
        write_capacity(out / "capacity.csv", capacity)
    finally:
        clear_netem(args, out)

    print(f"wrote {out / 'capacity.csv'}")
    print(f"wrote {out / 'summary.csv'}")
    return 0


def main() -> int:
    default_out = "results/final_saturation_profiles_" + dt.datetime.now(dt.timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    p = argparse.ArgumentParser()
    p.add_argument("--out", default=default_out)
    p.add_argument("--libraries", default=DEFAULT_LIBS)
    p.add_argument("--runs", default=DEFAULT_RUNS)
    p.add_argument("--netem", type=int, default=1)
    p.add_argument("--netem-args", default=DEFAULT_NETEM_ARGS)
    p.add_argument("--duration", default="20")
    p.add_argument("--tail-ms", default="500")
    p.add_argument("--idle", default="adaptive", choices=["spin", "adaptive"])
    p.add_argument("--isolate", default="systemd", choices=["taskset", "systemd"])
    p.add_argument("--server-cpu", default="7,15")
    p.add_argument("--client-cpu", default="5,6,13,14")
    p.add_argument("--build-dir", default="build")
    p.add_argument("--phase1-script", default="scripts/run_phase1_quick.sh")
    p.add_argument(
        "--litenetlib-bin",
        default="adapters/litenetlib/bin/Release/net10.0/litenetlib_adapter",
    )
    p.add_argument("--min-valid", type=int, default=0,
                   help="valid runs required per point; 0 means min(2, number of runs)")
    p.add_argument("--min-delivery", type=float, default=0.95)
    p.add_argument("--media-conns", default=DEFAULT_MEDIA_CONNS)
    p.add_argument("--game-conns", default=DEFAULT_GAME_CONNS)
    p.add_argument("--echo-conns", default=DEFAULT_ECHO_CONNS)
    p.add_argument("--reliable-echo-conns", default=DEFAULT_RELIABLE_ECHO_CONNS)
    p.add_argument("--echo-client-procs", type=int, default=4)
    args = p.parse_args()

    if not split_csv(args.libraries):
        raise SystemExit("--libraries must not be empty")
    runs = split_runs(args.runs)
    if not runs:
        raise SystemExit("--runs must not be empty")
    if args.min_valid <= 0:
        args.min_valid = min(2, len(runs))
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
