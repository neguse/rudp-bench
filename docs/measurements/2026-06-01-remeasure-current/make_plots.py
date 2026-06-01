#!/usr/bin/env python3
"""Plot the 2026-06-01 current remeasurement summary."""
from __future__ import annotations

import csv
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


HERE = Path(__file__).resolve().parent
DATA = HERE / "data" / "summary.csv"
PLOTS = HERE / "plots"

LIB_ORDER = ["enet", "kcp", "mini_rudp", "gns", "msquic", "litenetlib"]
LIB_COLOR = {
    "enet": "#d62728",
    "kcp": "#9467bd",
    "mini_rudp": "#1f77b4",
    "gns": "#2ca02c",
    "msquic": "#ff7f0e",
    "litenetlib": "#111111",
}
CONNS = [200, 600, 1000]


def fnum(value: str) -> float | None:
    if value == "":
        return None
    try:
        return float(value)
    except ValueError:
        return None


def load() -> dict[str, dict[int, dict[str, str]]]:
    rows: dict[str, dict[int, dict[str, str]]] = {lib: {} for lib in LIB_ORDER}
    with DATA.open(newline="") as f:
        for row in csv.DictReader(f):
            lib = row["library"]
            if lib not in rows:
                continue
            rows[lib][int(row["conns"])] = row
    return rows


def valid(row: dict[str, str] | None) -> bool:
    return bool(row and row.get("valid") == "1")


def plot_delivery(rows: dict[str, dict[int, dict[str, str]]]) -> None:
    fig, ax = plt.subplots(figsize=(9, 5))
    for lib in LIB_ORDER:
        xs: list[int] = []
        ys: list[float] = []
        for conns in CONNS:
            row = rows[lib].get(conns)
            if valid(row):
                value = fnum(row["delivery_ratio_median"])
                if value is not None:
                    xs.append(conns)
                    ys.append(value)
            elif row:
                ax.scatter([conns], [0.03], marker="x", color=LIB_COLOR[lib], s=60)
                note = row.get("note") or f"valid {row.get('n_valid', '?')}/3"
                ax.annotate(note, (conns, 0.06), ha="center", fontsize=7, rotation=35)
        ax.plot(xs, ys, "o-", color=LIB_COLOR[lib], label=lib, lw=2)
    ax.axhline(0.99, color="grey", lw=0.8, ls="--", label="~0.99 expected")
    ax.set_xlabel("connections")
    ax.set_ylabel("delivery_ratio median")
    ax.set_title("Current remeasurement: delivery vs connections")
    ax.set_xticks(CONNS)
    ax.set_ylim(0, 1.05)
    ax.grid(True, alpha=0.3)
    ax.legend(loc="lower left", framealpha=0.9, fontsize=8)
    fig.tight_layout()
    fig.savefig(PLOTS / "delivery_vs_conns.png", dpi=150)
    plt.close(fig)


def plot_cpu(rows: dict[str, dict[int, dict[str, str]]]) -> None:
    fig, ax = plt.subplots(figsize=(9, 5))
    for lib in LIB_ORDER:
        xs: list[int] = []
        ys: list[float] = []
        peaks: list[float] = []
        for conns in CONNS:
            row = rows[lib].get(conns)
            if valid(row):
                mean = fnum(row["server_cpu_pct_median"])
                peak = fnum(row["server_cpu_pct_peak_median"])
                if mean is not None and peak is not None:
                    xs.append(conns)
                    ys.append(mean)
                    peaks.append(peak)
        ax.plot(xs, ys, "o-", color=LIB_COLOR[lib], label=f"{lib} mean", lw=2)
        ax.plot(xs, peaks, ":", color=LIB_COLOR[lib], lw=1.2, alpha=0.8)
    ax.axhline(100, color="grey", lw=0.8, ls="--")
    ax.axhline(200, color="grey", lw=0.8, ls=":")
    ax.set_xlabel("connections")
    ax.set_ylabel("server CPU % median")
    ax.set_title("Current remeasurement: server CPU mean and peak")
    ax.set_xticks(CONNS)
    ax.set_ylim(0, 215)
    ax.grid(True, alpha=0.3)
    ax.legend(loc="upper left", framealpha=0.9, fontsize=8)
    fig.tight_layout()
    fig.savefig(PLOTS / "server_cpu_vs_conns.png", dpi=150)
    plt.close(fig)


def plot_rtt(rows: dict[str, dict[int, dict[str, str]]]) -> None:
    fig, axes = plt.subplots(1, 2, figsize=(13, 5))
    specs = [
        (axes[0], "rtt_r_p99_us_median", "reliable p99 RTT (ms)"),
        (axes[1], "rtt_u_p99_us_median", "unreliable p99 RTT (ms)"),
    ]
    for ax, key, ylabel in specs:
        for lib in LIB_ORDER:
            xs: list[int] = []
            ys: list[float] = []
            for conns in CONNS:
                row = rows[lib].get(conns)
                if valid(row):
                    value = fnum(row[key])
                    if value is not None:
                        xs.append(conns)
                        ys.append(value / 1000.0)
            ax.plot(xs, ys, "o-", color=LIB_COLOR[lib], label=lib, lw=2)
        ax.axhline(67, color="grey", lw=0.8, ls=":")
        ax.set_xlabel("connections")
        ax.set_ylabel(ylabel)
        ax.set_yscale("log")
        ax.set_xticks(CONNS)
        ax.grid(True, alpha=0.3, which="both")
        ax.legend(loc="upper left", framealpha=0.9, fontsize=8)
    axes[0].set_title("Reliable tail")
    axes[1].set_title("Unreliable tail")
    fig.suptitle("Current remeasurement: RTT p99")
    fig.tight_layout()
    fig.savefig(PLOTS / "rtt_p99_vs_conns.png", dpi=150)
    plt.close(fig)


def main() -> int:
    PLOTS.mkdir(exist_ok=True)
    rows = load()
    plot_delivery(rows)
    plot_cpu(rows)
    plot_rtt(rows)
    print("wrote:", ", ".join(sorted(p.name for p in PLOTS.glob("*.png"))))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
