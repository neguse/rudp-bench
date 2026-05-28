#!/usr/bin/env python3
"""Plot scale sweep: conns 200..1000 step 100, mixed (rate-r=50 + rate-u=50), loss=1%."""
from __future__ import annotations

import csv
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np


HERE = Path(__file__).resolve().parent
DATA = HERE / "data"
PLOTS = HERE / "plots"

CONNS = [200, 300, 400, 500, 600, 700, 800, 900, 1000]
LIB_ORDER = ["mini_rudp", "enet", "gns", "msquic"]
LIB_COLOR = {
    "mini_rudp": "#1f77b4",
    "enet":      "#d62728",
    "gns":       "#2ca02c",
    "msquic":    "#ff7f0e",
}


def load(prefix: str, conns: int) -> dict[str, dict]:
    path = DATA / f"{prefix}_l1_{conns}.csv"
    out: dict[str, dict] = {}
    with path.open() as f:
        for row in csv.DictReader(f):
            out[row["library"]] = row
    return out


def load_all(conns: int) -> dict[str, dict]:
    rows = load("scale_mix", conns)
    rows.update(load("scale_mix_n1", conns))
    return rows


def cell(row: dict, key: str, allow_invalid: bool = True) -> float | None:
    if row is None:
        return None
    v = row.get(key, "")
    if v in ("", "0", "0.0"):
        return None
    try:
        return float(v)
    except ValueError:
        return None


def plot_dr():
    fig, ax = plt.subplots(figsize=(9, 5))
    for lib in LIB_ORDER:
        ys = []
        for c in CONNS:
            row = load_all(c).get(lib)
            dr = cell(row, "delivery_ratio", allow_invalid=True)
            ys.append(dr)
        # crash points → break the line
        xs = [c for c, y in zip(CONNS, ys) if y is not None]
        ys_v = [y for y in ys if y is not None]
        ax.plot(xs, ys_v, "o-", color=LIB_COLOR[lib], label=lib, lw=2)
        # mark crash conns explicitly
        for c, y in zip(CONNS, ys):
            if y is None:
                row = load_all(c).get(lib)
                reason = row.get("invalid_reason", "?") if row else "?"
                ax.annotate(f"✗{reason}", xy=(c, 0.05), ha="center", fontsize=7,
                            color="red", rotation=45)
    ax.axhline(0.99, color="black", lw=0.5, ls="--", label="expected mix dr ≈ 0.99")
    ax.set_xlabel("conns")
    ax.set_ylabel("delivery_ratio")
    ax.set_ylim(0, 1.1)
    ax.set_xticks(CONNS)
    ax.grid(True, alpha=0.3)
    ax.legend(loc="lower left", framealpha=0.9)
    ax.set_title("Mixed (rate-r=50 + rate-u=50), loss=1%: dr vs conns")
    fig.tight_layout()
    fig.savefig(PLOTS / "dr_vs_conns.png", dpi=140)
    plt.close(fig)


def plot_rtt():
    fig, axes = plt.subplots(1, 2, figsize=(13, 5))
    for ax, key, label, ylim in [
        (axes[0], "rtt_r_p99_us", "reliable RTT p99 (ms, log)", (50, 30000)),
        (axes[1], "rtt_u_p99_us", "unreliable RTT p99 (ms, log)", (50, 5000)),
    ]:
        for lib in LIB_ORDER:
            ys = []
            xs = []
            for c in CONNS:
                row = load_all(c).get(lib)
                v = cell(row, key, allow_invalid=True)
                if v is not None:
                    xs.append(c)
                    ys.append(v / 1000.0)
            ax.plot(xs, ys, "o-", color=LIB_COLOR[lib], label=lib, lw=2)
        ax.set_xlabel("conns")
        ax.set_ylabel(label)
        ax.set_yscale("log")
        ax.set_ylim(*ylim)
        ax.set_xticks(CONNS)
        ax.set_xticklabels(CONNS, rotation=45)
        ax.grid(True, alpha=0.3, which="both")
        ax.legend(loc="upper left", framealpha=0.9, fontsize=8)
    axes[0].axhline(67, color="grey", lw=0.5, ls=":", label="baseline 67ms")
    axes[1].axhline(67, color="grey", lw=0.5, ls=":", label="baseline 67ms")
    axes[0].set_title("reliable retx tail")
    axes[1].set_title("unreliable channel (HoL view)")
    fig.suptitle("Mixed, loss=1%: RTT p99 vs conns")
    fig.tight_layout()
    fig.savefig(PLOTS / "rtt_vs_conns.png", dpi=140)
    plt.close(fig)


def main():
    PLOTS.mkdir(exist_ok=True)
    plot_dr()
    plot_rtt()
    print("wrote:", sorted(p.name for p in PLOTS.glob("*.png")))


if __name__ == "__main__":
    main()
