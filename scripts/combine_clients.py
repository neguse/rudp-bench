#!/usr/bin/env python3
"""Combine N parallel client raw CSVs + their RTT bin sidecars into one
client-shaped CSV by summing counts and recomputing percentiles from
merged histogram bins.

Mirrors harness/metrics.{h,cc} LatencyHist layout exactly. If you change
the C++ constants, change them here too — the test
tests/test_combine_clients.py asserts the percentiles agree with the
in-process LatencyHist output.
"""

from __future__ import annotations

import argparse
import csv
import struct
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple


# --- LatencyHist layout, mirrors harness/metrics.h kExact*/kFine*/kCoarse* ---
EXACT_MAX_US = 10_000
FINE_MAX_US = 1_000_000
COARSE_MAX_US = 60_000_000
FINE_BIN_US = 100
COARSE_BIN_US = 1_000
EXACT_BINS = EXACT_MAX_US + 1                                       # 10001
FINE_BINS = (FINE_MAX_US - EXACT_MAX_US + FINE_BIN_US - 1) // FINE_BIN_US  # 9900
COARSE_BINS = (COARSE_MAX_US - FINE_MAX_US + COARSE_BIN_US - 1) // COARSE_BIN_US  # 59000
BIN_COUNT = EXACT_BINS + FINE_BINS + COARSE_BINS                    # 78901

HEADER_STRUCT = struct.Struct("<IIQQQQ")  # magic, version, count, overflow, max, bin_count
MAGIC = 0x5453484C  # 'LHST'
VERSION = 1


class Histogram:
    __slots__ = ("count", "overflow", "max_us", "bins")

    def __init__(self) -> None:
        self.count: int = 0
        self.overflow: int = 0
        self.max_us: int = 0
        self.bins: List[int] = [0] * BIN_COUNT

    @classmethod
    def read(cls, path: Path) -> "Histogram":
        h = cls()
        with path.open("rb") as f:
            data = f.read()
        if len(data) < HEADER_STRUCT.size + 8 * BIN_COUNT:
            raise ValueError(f"{path}: file too short ({len(data)} bytes)")
        magic, version, count, overflow, max_us, bin_count = HEADER_STRUCT.unpack_from(data, 0)
        if magic != MAGIC:
            raise ValueError(f"{path}: bad magic 0x{magic:08x}")
        if version != VERSION:
            raise ValueError(f"{path}: unsupported version {version}")
        if bin_count != BIN_COUNT:
            raise ValueError(
                f"{path}: bin_count {bin_count} != expected {BIN_COUNT} "
                f"(LatencyHist layout drift?)"
            )
        h.count = count
        h.overflow = overflow
        h.max_us = max_us
        h.bins = list(struct.unpack_from(f"<{BIN_COUNT}Q", data, HEADER_STRUCT.size))
        return h

    def merge_from(self, other: "Histogram") -> None:
        self.count += other.count
        self.overflow += other.overflow
        if other.max_us > self.max_us:
            self.max_us = other.max_us
        for i, c in enumerate(other.bins):
            if c:
                self.bins[i] += c

    @staticmethod
    def bin_upper_bound_us(index: int) -> int:
        # Mirrors LatencyHist::bin_upper_bound_us in metrics.cc.
        if index < EXACT_BINS:
            return index
        if index < EXACT_BINS + FINE_BINS:
            fine_index = index - EXACT_BINS
            return EXACT_MAX_US + (fine_index + 1) * FINE_BIN_US
        coarse_index = index - EXACT_BINS - FINE_BINS
        return FINE_MAX_US + (coarse_index + 1) * COARSE_BIN_US

    def percentile_us(self, p: float) -> int:
        # Mirrors LatencyHist::percentile_us in metrics.cc.
        if self.count == 0:
            return 0
        q = max(0.0, min(1.0, p))
        target = int(q * float(self.count - 1)) + 1
        seen = 0
        for i, c in enumerate(self.bins):
            seen += c
            if seen >= target:
                return self.bin_upper_bound_us(i)
        return self.max_us


# --- CSV row aggregation ---

# Columns aggregated by summation across N client CSVs.
SUM_COLS = [
    "delivered",
    "accepted",
    "client_attempted",
    "client_accepted",
    "client_missed_pacing",
]

# Columns aggregated by max across N client CSVs (worst observed).
MAX_COLS = [
    "rss_mb",
    "connect_ms",
    "close_ms",
    "cpu_pct_peak",
    "client_tick_gap_p99_us",
    "client_tick_gap_max_us",
    "client_pacing_lag_p99_us",
    "client_pacing_lag_max_us",
    "client_recv_drained_p99",
    "client_recv_drained_max",
    "client_outstanding_max",
    "rtt_r_p50_us",
    "rtt_r_p95_us",
    "rtt_r_p99_us",
    "rtt_u_p50_us",
    "rtt_u_p95_us",
    "rtt_u_p99_us",
]

# Columns aggregated by sum then re-derived ratios after.
PASSTHROUGH_FROM_FIRST = [
    "library",
    "encryption",
    "phase",
    "rate_r",
    "rate_u",
    "size",
    "loss",
    "duration_s",
    "mode",
    "idle_policy",
    "flush_policy",
    "delivery_dedup_policy",
]


def to_int(s: str) -> int:
    if s is None or s == "":
        return 0
    try:
        return int(float(s))
    except ValueError:
        return 0


def to_float(s: str) -> float:
    if s is None or s == "":
        return 0.0
    try:
        return float(s)
    except ValueError:
        return 0.0


def attempted_target(row: Dict[str, str]) -> float:
    combined_rate = to_int(row.get("rate_r", "")) + to_int(row.get("rate_u", ""))
    conns = to_int(row.get("conns", ""))
    duration_s = to_int(row.get("duration_s", ""))
    if combined_rate > 0 and conns > 0 and duration_s > 0:
        expected_per_send = conns if row.get("mode") == "broadcast" else 1
        return float(combined_rate * conns * duration_s * expected_per_send)

    attempted = to_int(row.get("client_attempted", ""))
    attempted_ratio = to_float(row.get("client_attempted_ratio", ""))
    if attempted_ratio > 0.0:
        return attempted / attempted_ratio
    return float(attempted)


def read_csv(path: Path) -> Dict[str, str]:
    with path.open(newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        raise ValueError(f"{path}: empty CSV")
    return rows[0]


def combine_throughput(rows: List[Dict[str, str]]) -> Tuple[float, int]:
    duration_s = max(to_int(r.get("duration_s", "")) for r in rows) or 1
    bytes_sum = 0
    msgs_sum = 0
    for r in rows:
        msgs = to_int(r.get("msg_per_sec", "")) * duration_s
        # msg size from row size column
        size = to_int(r.get("size", ""))
        bytes_sum += msgs * size
        msgs_sum += msgs
    mbps = bytes_sum * 8.0 / (duration_s * 1_000_000.0)
    return mbps, msgs_sum // duration_s


def combine(
    client_csvs: List[Path],
    client_bins_r: List[Path],
    client_bins_u: List[Path],
    out: Path,
    procs: int,
    conns_total: int,
) -> None:
    if not client_csvs:
        raise SystemExit("no client csvs provided")
    rows = [read_csv(p) for p in client_csvs]
    first = rows[0]

    hist_r = Histogram()
    for p in client_bins_r:
        hist_r.merge_from(Histogram.read(p))
    hist_u = Histogram()
    for p in client_bins_u:
        hist_u.merge_from(Histogram.read(p))

    combined: Dict[str, str] = {}
    for col in PASSTHROUGH_FROM_FIRST:
        combined[col] = first.get(col, "")
    combined["conns"] = str(conns_total)

    for col in SUM_COLS:
        combined[col] = str(sum(to_int(r.get(col, "")) for r in rows))
    for col in MAX_COLS:
        combined[col] = str(max(to_int(r.get(col, "")) for r in rows))

    # Recompute percentiles from merged histogram bins.
    combined["rtt_r_p50_us"] = str(hist_r.percentile_us(0.50))
    combined["rtt_r_p95_us"] = str(hist_r.percentile_us(0.95))
    combined["rtt_r_p99_us"] = str(hist_r.percentile_us(0.99))
    combined["rtt_u_p50_us"] = str(hist_u.percentile_us(0.50))
    combined["rtt_u_p95_us"] = str(hist_u.percentile_us(0.95))
    combined["rtt_u_p99_us"] = str(hist_u.percentile_us(0.99))

    # Aggregate throughput from individual rows.
    mbps, msg_per_sec = combine_throughput(rows)
    combined["throughput_mbps"] = f"{mbps:.3f}"
    combined["msg_per_sec"] = str(msg_per_sec)

    # CPU: sum across procs (total CPU used). RSS already max above.
    combined["cpu_pct"] = f"{sum(to_float(r.get('cpu_pct', '')) for r in rows):.2f}"

    # Ratios re-derived from summed totals.
    attempted = to_int(combined["client_attempted"])
    accepted = to_int(combined["client_accepted"])
    delivered = to_int(combined["delivered"])
    target_attempted = sum(attempted_target(r) for r in rows)
    combined["client_attempted_ratio"] = (
        f"{attempted / target_attempted:.4f}" if target_attempted else "0.0000"
    )
    combined["client_accepted_ratio"] = (
        f"{accepted / attempted:.4f}" if attempted else "0.0000"
    )
    combined["delivery_ratio"] = (
        f"{delivered / accepted:.4f}" if accepted else "0.0000"
    )

    # tick_ok from the AGGREGATE ratios (functional correctness of the whole
    # farm), not an AND of per-proc flags. ANDing per-proc tick_ok is too
    # strict once conns are split across many procs: a single proc at 0.989
    # attempted fails the whole run even though the farm applied 99.2% of the
    # offered load (observed with litenetlib at 1000 conns / 6 procs). The
    # aggregate attempted/accepted ratios are what define a trustworthy run,
    # matching the per-binary gate in harness/runner.cc (tick_gap is a
    # diagnostic, not a gate).
    agg_attempted = to_float(combined["client_attempted_ratio"])
    agg_accepted = to_float(combined["client_accepted_ratio"])
    combined["client_tick_ok"] = (
        "1" if agg_attempted >= 0.99 and agg_accepted >= 0.99 else "0"
    )

    # Emit the combined row using the header from the first input csv.
    with client_csvs[0].open(newline="") as f:
        reader = csv.reader(f)
        header = next(reader)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=header)
        writer.writeheader()
        writer.writerow({k: combined.get(k, first.get(k, "")) for k in header})


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--client-csv", action="append", required=True,
                   help="path to one client raw CSV (repeat per proc)")
    p.add_argument("--bins-r", action="append", required=True,
                   help="path to one client's rtt_r bin file (repeat per proc)")
    p.add_argument("--bins-u", action="append", required=True,
                   help="path to one client's rtt_u bin file (repeat per proc)")
    p.add_argument("--out", required=True, type=Path,
                   help="combined client CSV path")
    p.add_argument("--conns-total", required=True, type=int,
                   help="total connection count across all client procs")
    args = p.parse_args()

    csvs = [Path(p) for p in args.client_csv]
    bins_r = [Path(p) for p in args.bins_r]
    bins_u = [Path(p) for p in args.bins_u]
    if not (len(csvs) == len(bins_r) == len(bins_u)):
        raise SystemExit("--client-csv / --bins-r / --bins-u must have the same count")

    combine(csvs, bins_r, bins_u, args.out, procs=len(csvs), conns_total=args.conns_total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
