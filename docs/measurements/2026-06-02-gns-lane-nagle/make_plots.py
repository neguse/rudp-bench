#!/usr/bin/env python3
from __future__ import annotations

import csv
from pathlib import Path

import matplotlib.pyplot as plt


ROOT = Path(__file__).resolve().parent
DATA = ROOT / "data" / "summary.csv"
PLOTS = ROOT / "plots"

ORDER = ["gns_split_no_nagle", "gns_split_nagle"]
LABELS = {
    "gns_split_no_nagle": "split lanes + NoNagle",
    "gns_split_nagle": "split lanes + Nagle",
}
COLORS = {
    "gns_split_no_nagle": "#1f77b4",
    "gns_split_nagle": "#d62728",
}


def load_rows() -> list[dict[str, str]]:
    with DATA.open(newline="") as f:
        return list(csv.DictReader(f))


def by_library(rows: list[dict[str, str]]) -> dict[str, list[dict[str, str]]]:
    out: dict[str, list[dict[str, str]]] = {}
    for row in rows:
        if row.get("valid") != "1":
            continue
        out.setdefault(row["library"], []).append(row)
    for lib_rows in out.values():
        lib_rows.sort(key=lambda r: int(r["conns"]))
    return out


def f(row: dict[str, str], name: str) -> float:
    return float(row[name])


def plot_metric(rows_by_lib: dict[str, list[dict[str, str]]], metric: str,
                ylabel: str, path: Path) -> None:
    fig, ax = plt.subplots(figsize=(7, 4))
    for lib in ORDER:
        lib_rows = rows_by_lib.get(lib, [])
        ax.plot(
            [int(r["conns"]) for r in lib_rows],
            [f(r, metric) for r in lib_rows],
            marker="o",
            linewidth=2,
            color=COLORS[lib],
            label=LABELS[lib],
        )
    ax.set_xlabel("connections")
    ax.set_ylabel(ylabel)
    ax.grid(True, alpha=0.25)
    ax.legend()
    fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)


def plot_rtt(rows_by_lib: dict[str, list[dict[str, str]]], path: Path) -> None:
    fig, axes = plt.subplots(1, 2, figsize=(10, 4), sharex=True)
    for ax, metric, title in [
        (axes[0], "rtt_r_p99_us_median", "reliable p99"),
        (axes[1], "rtt_u_p99_us_median", "unreliable p99"),
    ]:
        for lib in ORDER:
            lib_rows = rows_by_lib.get(lib, [])
            ax.plot(
                [int(r["conns"]) for r in lib_rows],
                [f(r, metric) / 1000.0 for r in lib_rows],
                marker="o",
                linewidth=2,
                color=COLORS[lib],
                label=LABELS[lib],
            )
        ax.set_title(title)
        ax.set_xlabel("connections")
        ax.set_ylabel("ms")
        ax.grid(True, alpha=0.25)
    axes[0].legend()
    fig.tight_layout()
    fig.savefig(path, dpi=160)
    plt.close(fig)


def main() -> int:
    PLOTS.mkdir(parents=True, exist_ok=True)
    rows_by_lib = by_library(load_rows())
    plot_metric(
        rows_by_lib,
        "delivery_ratio_median",
        "end-to-end delivery ratio",
        PLOTS / "delivery_vs_conns.png",
    )
    plot_metric(
        rows_by_lib,
        "forward_delivery_ratio_u_median",
        "client-to-server unreliable delivery",
        PLOTS / "forward_unreliable_vs_conns.png",
    )
    plot_metric(
        rows_by_lib,
        "server_cpu_pct_median",
        "server CPU %",
        PLOTS / "server_cpu_vs_conns.png",
    )
    plot_rtt(rows_by_lib, PLOTS / "rtt_p99_vs_conns.png")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
