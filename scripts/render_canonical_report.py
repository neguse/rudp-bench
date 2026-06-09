#!/usr/bin/env python3
"""Render a Markdown report with embedded plots for a canonical benchmark run."""

from __future__ import annotations

import argparse
import csv
import datetime as dt
import math
import os
import re
import sys
from pathlib import Path
from typing import Callable, Dict, List, Optional, Sequence, Tuple

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


SCENARIO_RE = re.compile(
    r"^(?P<library>.+)_r(?P<rate_r>\d+)_u(?P<rate_u>\d+)_"
    r"(?P<size>\d+)_(?P<conns>\d+)_(?P<mode>echo|broadcast)_"
    r"(?P<loss>[^_]+)_(?P<idle>.+)$"
)

DEFAULT_MEDIA_CONNS = "1 5 50 75 100 125 150 200"
DEFAULT_GAME_CONNS = "1 5 64 96 128 192 256"
DEFAULT_ECHO_CONNS = "1 50 200 600 1000 1500 2000 3000"
DEFAULT_RELIABLE_ECHO_CONNS = "1 50 200 600 1000 1500 2000 3000"


DEFAULT_PROFILE_ROWS = [
    {
        "profile": "media_relay",
        "use_case": "media_sfu_unreliable_fanout",
        "mode": "broadcast",
        "rate_r": "0",
        "rate_u": "30",
        "size": "1000",
        "conns_schedule": DEFAULT_MEDIA_CONNS,
        "client_procs": "4",
        "notes": "near-MTU media packets, full-room unreliable fanout",
    },
    {
        "profile": "game_server",
        "use_case": "authoritative_game_snapshot_event_fanout",
        "mode": "broadcast",
        "rate_r": "1",
        "rate_u": "20",
        "size": "128",
        "conns_schedule": DEFAULT_GAME_CONNS,
        "client_procs": "4",
        "notes": "20Hz state/input fanout plus 1Hz reliable gameplay events",
    },
    {
        "profile": "reliable_echo",
        "use_case": "reliable_transport_echo_baseline",
        "mode": "echo",
        "rate_r": "50",
        "rate_u": "0",
        "size": "64",
        "conns_schedule": DEFAULT_RELIABLE_ECHO_CONNS,
        "client_procs": "4",
        "notes": "reliable-only echo baseline for stream/reliable transports",
    },
    {
        "profile": "echo",
        "use_case": "synthetic_mixed_echo_baseline",
        "mode": "echo",
        "rate_r": "50",
        "rate_u": "50",
        "size": "64",
        "conns_schedule": DEFAULT_ECHO_CONNS,
        "client_procs": "4",
        "notes": "mixed 50/50 echo baseline used for implementation validation",
    },
]


def read_csv(path: Path) -> List[Dict[str, str]]:
    if not path.exists():
        raise FileNotFoundError(path)
    with path.open(newline="") as f:
        return list(csv.DictReader(f))


def to_float(value: object) -> Optional[float]:
    if value is None or value == "":
        return None
    try:
        out = float(str(value))
    except ValueError:
        return None
    if math.isnan(out):
        return None
    return out


def to_int(value: object) -> Optional[int]:
    f = to_float(value)
    if f is None:
        return None
    return int(f)


def fmt(value: object, digits: int = 4) -> str:
    f = to_float(value)
    if f is None:
        return ""
    if abs(f - round(f)) < 0.0000001:
        return str(int(round(f)))
    return f"{f:.{digits}f}"


def split_conns(value: str) -> List[int]:
    out: List[int] = []
    for item in value.replace(",", " ").split():
        parsed = to_int(item)
        if parsed is not None:
            out.append(parsed)
    return out


def markdown_table(headers: Sequence[str], rows: Sequence[Sequence[object]]) -> str:
    def cell(value: object) -> str:
        text = "" if value is None else str(value)
        return text.replace("|", "\\|")

    lines = [
        "| " + " | ".join(cell(h) for h in headers) + " |",
        "| " + " | ".join("---" for _ in headers) + " |",
    ]
    for row in rows:
        lines.append("| " + " | ".join(cell(v) for v in row) + " |")
    return "\n".join(lines)


def load_profiles(run_dir: Path) -> Tuple[List[Dict[str, str]], Dict[Tuple[str, str, str, str], str]]:
    path = run_dir / "profiles.csv"
    rows = read_csv(path) if path.exists() else list(DEFAULT_PROFILE_ROWS)
    key_map: Dict[Tuple[str, str, str, str], str] = {}
    for row in rows:
        key = (
            row.get("mode", ""),
            row.get("rate_r", ""),
            row.get("rate_u", ""),
            row.get("size", ""),
        )
        key_map[key] = row.get("profile", "")
    return rows, key_map


def profile_order(profile_rows: Sequence[Dict[str, str]]) -> List[str]:
    return [row.get("profile", "") for row in profile_rows if row.get("profile")]


def annotate_summary_rows(
    rows: Sequence[Dict[str, str]],
    profile_by_shape: Dict[Tuple[str, str, str, str], str],
) -> List[Dict[str, str]]:
    out: List[Dict[str, str]] = []
    for row in rows:
        item = dict(row)
        match = SCENARIO_RE.match(row.get("scenario_id", ""))
        if match:
            key = (
                match.group("mode"),
                match.group("rate_r"),
                match.group("rate_u"),
                match.group("size"),
            )
            item["_profile"] = profile_by_shape.get(key, "")
            item["_conns"] = match.group("conns")
        else:
            item["_profile"] = ""
            item["_conns"] = row.get("conns", "")
        if not item.get("_conns"):
            item["_conns"] = row.get("conns", "")
        out.append(item)
    return out


def libraries(capacity_rows: Sequence[Dict[str, str]], summary_rows: Sequence[Dict[str, str]]) -> List[str]:
    seen = []
    for row in list(capacity_rows) + list(summary_rows):
        lib = row.get("library", "")
        if lib and lib not in seen:
            seen.append(lib)
    return seen


def strongest_rows(
    capacity_rows: Sequence[Dict[str, str]],
    profiles: Sequence[str],
) -> List[Tuple[str, str, str, str, str]]:
    rows: List[Tuple[str, str, str, str, str]] = []
    for profile in profiles:
        candidates = [row for row in capacity_rows if row.get("profile") == profile]
        best = None
        best_key = (-1, float("inf"))
        for row in candidates:
            ok = to_int(row.get("last_ok_conns"))
            if ok is None:
                continue
            cpu = to_float(row.get("last_ok_server_cpu"))
            key = (ok, -(cpu if cpu is not None else 999999.0))
            if key > best_key:
                best = row
                best_key = key
        if best is None:
            rows.append((profile, "unmeasured", "unmeasured", "unmeasured", ""))
            continue
        break_text = "not broken"
        if best.get("break_conns"):
            break_text = f"{best.get('break_conns')} ({best.get('break_reason', '')})"
        rows.append(
            (
                profile,
                best.get("library", ""),
                best.get("last_ok_conns", ""),
                break_text,
                f"delivery {best.get('last_ok_delivery', '')}, CPU {best.get('last_ok_server_cpu', '')}%",
            )
        )
    return rows


def display_last_ok(row: Dict[str, str]) -> str:
    value = row.get("last_ok_conns", "")
    if value:
        return value
    status = row.get("status", "")
    if status in {"unsupported", "below_gate", "failed_gate"}:
        return status
    return "unmeasured"


def save_capacity_plot(
    capacity_rows: Sequence[Dict[str, str]],
    profiles: Sequence[str],
    libs: Sequence[str],
    out: Path,
) -> Optional[Path]:
    if not capacity_rows:
        return None
    fig_height = max(4.8, 2.2 * len(profiles))
    fig, axes = plt.subplots(len(profiles), 1, figsize=(9.5, fig_height), sharex=False)
    if len(profiles) == 1:
        axes = [axes]
    colors = plt.rcParams["axes.prop_cycle"].by_key().get("color", [])
    for ax, profile in zip(axes, profiles):
        values: List[float] = []
        labels: List[str] = []
        for lib in libs:
            row = next(
                (r for r in capacity_rows if r.get("profile") == profile and r.get("library") == lib),
                None,
            )
            value = float(to_int(row.get("last_ok_conns")) or 0) if row else 0.0
            values.append(value)
            labels.append(lib)
        y = list(range(len(labels)))
        bar_colors = [colors[i % len(colors)] if colors else None for i in y]
        ax.barh(y, values, color=bar_colors)
        ax.set_title(f"{profile}: max OK connections")
        ax.set_yticks(y)
        ax.set_yticklabels(labels)
        ax.invert_yaxis()
        ax.grid(axis="x", alpha=0.28)
        ax.set_xlabel("max OK connections")
        xmax = max(values) if values else 0
        ax.set_xlim(0, xmax * 1.18 if xmax > 0 else 1)
        for yi, value, lib in zip(y, values, labels):
            row = next(
                (r for r in capacity_rows if r.get("profile") == profile and r.get("library") == lib),
                None,
            )
            if row and row.get("status") in {"unsupported", "below_gate", "failed_gate"}:
                label = row.get("status", "")
            elif value <= 0:
                label = "unmeasured"
            else:
                label = str(int(value))
            xpos = value + max(xmax * 0.02, 0.5) if value > 0 else max(xmax * 0.02, 0.5)
            ax.text(xpos, yi, label, va="center", fontsize=8)
    fig.tight_layout()
    fig.savefig(out, dpi=140, bbox_inches="tight")
    plt.close(fig)
    return out


def numeric_points(
    rows: Sequence[Dict[str, str]],
    profile: str,
    lib: str,
    value_fn: Callable[[Dict[str, str]], Optional[float]],
) -> Tuple[List[int], List[float]]:
    points: List[Tuple[int, float]] = []
    for row in rows:
        if row.get("_profile") != profile or row.get("library") != lib:
            continue
        conns = to_int(row.get("_conns") or row.get("conns"))
        value = value_fn(row)
        if conns is None or value is None:
            continue
        points.append((conns, value))
    points.sort(key=lambda item: item[0])
    return [p[0] for p in points], [p[1] for p in points]


def metric_plot(
    rows: Sequence[Dict[str, str]],
    profile: str,
    libs: Sequence[str],
    value_fn: Callable[[Dict[str, str]], Optional[float]],
    ylabel: str,
    title: str,
    out: Path,
    threshold: Optional[float] = None,
) -> Optional[Path]:
    fig, ax = plt.subplots(figsize=(8, 4.8))
    plotted = False
    for lib in libs:
        xs, ys = numeric_points(rows, profile, lib, value_fn)
        if not xs:
            continue
        plotted = True
        ax.plot(xs, ys, marker="o", linewidth=1.8, label=lib)
    if not plotted:
        plt.close(fig)
        return None
    if threshold is not None:
        ax.axhline(threshold, linestyle="--", linewidth=1.2, color="black", alpha=0.55)
        ax.text(0.995, threshold, f" {threshold:g}", transform=ax.get_yaxis_transform(), va="bottom", ha="right")
    ax.set_title(title)
    ax.set_xlabel("connections")
    ax.set_ylabel(ylabel)
    ax.grid(True, alpha=0.28)
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(out, dpi=140, bbox_inches="tight")
    plt.close(fig)
    return out


def rtt_p95_ms(row: Dict[str, str]) -> Optional[float]:
    unreliable = to_float(row.get("rtt_u_p95_us_median"))
    reliable = to_float(row.get("rtt_r_p95_us_median"))
    chosen = unreliable if unreliable and unreliable > 0 else reliable
    if chosen is None:
        return None
    return chosen / 1000.0


def render_plots(
    capacity_rows: Sequence[Dict[str, str]],
    summary_rows: Sequence[Dict[str, str]],
    profiles: Sequence[str],
    libs: Sequence[str],
    plots_dir: Path,
) -> Dict[str, List[Path]]:
    plots_dir.mkdir(parents=True, exist_ok=True)
    out: Dict[str, List[Path]] = {"_top": []}
    top = save_capacity_plot(capacity_rows, profiles, libs, plots_dir / "capacity_max_ok.png")
    if top:
        out["_top"].append(top)
    for profile in profiles:
        profile_plots: List[Path] = []
        for path in [
            metric_plot(
                summary_rows,
                profile,
                libs,
                lambda row: to_float(row.get("delivery_ratio_median")),
                "median delivery ratio",
                f"{profile}: delivery ratio vs connections",
                plots_dir / f"{profile}_delivery.png",
                threshold=0.95,
            ),
            metric_plot(
                summary_rows,
                profile,
                libs,
                lambda row: to_float(row.get("server_cpu_pct_median")),
                "median server CPU %",
                f"{profile}: server CPU vs connections",
                plots_dir / f"{profile}_server_cpu.png",
            ),
            metric_plot(
                summary_rows,
                profile,
                libs,
                rtt_p95_ms,
                "median RTT p95 (ms)",
                f"{profile}: RTT p95 vs connections",
                plots_dir / f"{profile}_rtt_p95.png",
            ),
        ]:
            if path:
                profile_plots.append(path)
        out[profile] = profile_plots
    return out


def rel(path: Path, start: Path) -> str:
    return os.path.relpath(path, start).replace(os.sep, "/")


def build_report(
    run_dir: Path,
    out_path: Path,
    run_label: str,
    profile_rows: Sequence[Dict[str, str]],
    capacity_rows: Sequence[Dict[str, str]],
    summary_rows: Sequence[Dict[str, str]],
    plots: Dict[str, List[Path]],
) -> str:
    profiles = profile_order(profile_rows)
    generated = dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    strongest = strongest_rows(capacity_rows, profiles)
    lines: List[str] = [
        "# Canonical Benchmark Report",
        "",
        f"Generated: {generated}",
        "",
        f"Result directory: `{run_label}`",
        "",
        "This report is generated by `scripts/run_canonical_tests.sh`. It is the first file to open after a canonical benchmark run.",
        "",
        "## Verdict",
        "",
        markdown_table(
            ["profile", "strongest", "max OK", "break", "max OK readout"],
            strongest,
        ),
        "",
        "OK means aggregate valid runs meet the gate and median `delivery_ratio >= 0.95`.",
        "",
        "## Graphs",
        "",
    ]
    for path in plots.get("_top", []):
        lines.extend([f"![Max OK capacity]({rel(path, out_path.parent)})", ""])
    for profile in profiles:
        lines.extend([f"### `{profile}`", ""])
        if not plots.get(profile):
            lines.extend(["No graphable rows.", ""])
            continue
        for path in plots[profile]:
            title = path.stem.replace("_", " ")
            lines.extend([f"![{title}]({rel(path, out_path.parent)})", ""])
    lines.extend(
        [
            "## Capacity Table",
            "",
            markdown_table(
                [
                    "profile",
                    "library",
                    "status",
                    "last OK",
                    "last OK delivery",
                    "last OK CPU",
                    "break",
                    "break reason",
                    "break delivery",
                    "break CPU",
                ],
                [
                    [
                        row.get("profile", ""),
                        row.get("library", ""),
                        row.get("status", ""),
                        display_last_ok(row),
                        row.get("last_ok_delivery", ""),
                        row.get("last_ok_server_cpu", ""),
                        row.get("break_conns", "") or "not broken",
                        row.get("break_reason", ""),
                        row.get("break_delivery", ""),
                        row.get("break_server_cpu", ""),
                    ]
                    for row in capacity_rows
                ],
            ),
            "",
            "## Profiles",
            "",
            markdown_table(
                ["profile", "mode", "traffic", "payload", "conn sweep", "client procs"],
                [
                    [
                        row.get("profile", ""),
                        row.get("mode", ""),
                        f"r{row.get('rate_r', '')}/u{row.get('rate_u', '')}",
                        row.get("size", ""),
                        row.get("conns_schedule", ""),
                        row.get("client_procs", ""),
                    ]
                    for row in profile_rows
                ],
            ),
            "",
            "## Data Files",
            "",
            f"- [`capacity.csv`]({rel(run_dir / 'capacity.csv', out_path.parent)})",
            f"- [`summary.csv`]({rel(run_dir / 'summary.csv', out_path.parent)})",
            f"- [`results_all.csv`]({rel(run_dir / 'results_all.csv', out_path.parent)})",
            f"- [`scenarios_all.csv`]({rel(run_dir / 'scenarios_all.csv', out_path.parent)})",
            f"- [`profiles.csv`]({rel(run_dir / 'profiles.csv', out_path.parent)})",
            "",
        ]
    )
    return "\n".join(lines)


def render(args: argparse.Namespace) -> int:
    run_dir = Path(args.run_dir).resolve()
    out_path = Path(args.out).resolve() if args.out else run_dir / "report.md"
    plots_dir = Path(args.plots_dir).resolve() if args.plots_dir else run_dir / "plots"
    run_label = args.run_label or str(run_dir)
    profile_rows, profile_by_shape = load_profiles(run_dir)
    capacity_rows = read_csv(run_dir / "capacity.csv")
    raw_summary_rows = read_csv(run_dir / "summary.csv")
    summary_rows = annotate_summary_rows(raw_summary_rows, profile_by_shape)
    profiles = profile_order(profile_rows)
    libs = libraries(capacity_rows, summary_rows)
    plots = render_plots(capacity_rows, summary_rows, profiles, libs, plots_dir)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(
        build_report(run_dir, out_path, run_label, profile_rows, capacity_rows, summary_rows, plots),
        encoding="utf-8",
    )
    print(f"wrote {out_path}")
    for paths in plots.values():
        for path in paths:
            print(f"wrote {path}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", required=True, help="Directory containing capacity.csv and summary.csv")
    parser.add_argument("--out", default="", help="Markdown output path; default: RUN_DIR/report.md")
    parser.add_argument("--plots-dir", default="", help="Plot output directory; default: RUN_DIR/plots")
    parser.add_argument("--run-label", default="", help="Label to print in the report; default: absolute RUN_DIR")
    args = parser.parse_args()
    try:
        return render(args)
    except FileNotFoundError as exc:
        print(f"missing required input: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
