#!/usr/bin/env python3
"""Aggregate N repeated runs per scenario into medians over the valid runs.

S1: reduce_result.py normalizes ONE run; it has no median / valid-selection
logic, so the published summary.csv was hand-baked ("手で焼き込み") and therefore
not reproducible or auditable. This script consumes the multi-run results.csv
that `reduce_result.py append` produces (one row per run_id x scenario_id),
selects the valid runs, and emits the median of each metric PLUS exactly which
run_ids were included and how many of N were valid — so the summary is
re-derivable from raw data instead of typed by hand.

Usage:
  scripts/aggregate_runs.py --results results.csv --scenarios scenarios.csv \
      --out summary.csv [--min-valid 2]

`--scenarios` is optional and only used to attach a `conns` column. `--min-valid`
is the number of valid runs required to call the aggregate itself valid
(default 1; for N=3 with a median, 2 is the usual gate).
"""

import argparse
import csv
import statistics
from pathlib import Path
from typing import Dict, List, Optional

# column -> decimal places in the emitted median ("" = round to int)
METRIC_FORMAT = {
    "delivery_ratio": 4,
    "server_cpu_pct": 2,
    "rtt_r_p50_us": 0,
    "rtt_r_p95_us": 0,
    "rtt_r_p99_us": 0,
    "rtt_u_p50_us": 0,
    "rtt_u_p95_us": 0,
    "rtt_u_p99_us": 0,
}


def fnum(s: object) -> Optional[float]:
    try:
        if s is None or s == "":
            return None
        return float(str(s))
    except ValueError:
        return None


def median_str(values: List[Optional[float]], places: int) -> str:
    vals = [v for v in values if v is not None]
    if not vals:
        return ""
    m = statistics.median(vals)
    if places == 0:
        return str(int(round(m)))
    return f"{m:.{places}f}"


def load_conns(scenarios_path: str) -> Dict[tuple, str]:
    out: Dict[tuple, str] = {}
    if scenarios_path and Path(scenarios_path).exists():
        with open(scenarios_path, newline="") as f:
            for r in csv.DictReader(f):
                out[(r.get("library", ""), r.get("scenario_id", ""))] = r.get("conns", "")
    return out


def dominant(values: List[str]) -> str:
    values = [v for v in values if v]
    if not values:
        return ""
    return max(set(values), key=values.count)


def aggregate(results_path: str, scenarios_path: str, out_path: str,
              min_valid: int) -> int:
    conns_map = load_conns(scenarios_path)
    groups: Dict[tuple, List[Dict[str, str]]] = {}
    with open(results_path, newline="") as f:
        for r in csv.DictReader(f):
            groups.setdefault((r.get("library", ""), r.get("scenario_id", "")), []).append(r)

    metric_cols = list(METRIC_FORMAT.keys())
    fields = (["library", "scenario_id", "conns", "n_total", "n_valid", "valid"]
              + [c + "_median" for c in metric_cols]
              + ["included_run_ids", "note"])

    out_rows: List[Dict[str, object]] = []
    for (lib, sid), rows in sorted(groups.items()):
        valid_rows = [r for r in rows if r.get("valid") == "1"]
        agg: Dict[str, object] = {
            "library": lib,
            "scenario_id": sid,
            "conns": conns_map.get((lib, sid), ""),
            "n_total": len(rows),
            "n_valid": len(valid_rows),
            "valid": "1" if len(valid_rows) >= min_valid else "0",
            "included_run_ids": ";".join(r.get("run_id", "") for r in valid_rows),
            "note": "" if valid_rows else dominant([r.get("invalid_reason", "") for r in rows]),
        }
        for col, places in METRIC_FORMAT.items():
            agg[col + "_median"] = median_str([fnum(r.get(col)) for r in valid_rows], places)
        out_rows.append(agg)

    Path(out_path).parent.mkdir(parents=True, exist_ok=True)
    with open(out_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for r in out_rows:
            w.writerow({k: r.get(k, "") for k in fields})
    print(f"aggregate_runs: {len(out_rows)} scenario groups -> {out_path}")
    return 0


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--results", required=True)
    p.add_argument("--scenarios", default="")
    p.add_argument("--out", required=True)
    p.add_argument("--min-valid", type=int, default=1)
    args = p.parse_args()
    return aggregate(args.results, args.scenarios, args.out, args.min_valid)


if __name__ == "__main__":
    raise SystemExit(main())
