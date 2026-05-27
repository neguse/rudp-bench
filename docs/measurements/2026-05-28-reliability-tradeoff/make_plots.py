#!/usr/bin/env python3
"""Render reliability tradeoff plots from the matched unreliable/reliable runs.

Inputs are the CSVs under ./data/ (copied from results/). Outputs PNGs under
./plots/. Run from this directory: `python3 make_plots.py`.
"""
from __future__ import annotations

import csv
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np


HERE = Path(__file__).resolve().parent
DATA = HERE / "data"
PLOTS = HERE / "plots"


LIB_ORDER = ["raw_udp", "mini_rudp", "enet"]
LIB_COLOR = {"raw_udp": "#888888", "mini_rudp": "#1f77b4", "enet": "#d62728"}


def load(prefix: str, loss: int, conns: int) -> dict[str, dict]:
    path = DATA / f"{prefix}_l{loss}_{conns}.csv"
    out: dict[str, dict] = {}
    with path.open() as f:
        for row in csv.DictReader(f):
            out[row["library"]] = row
    return out


def cell(row: dict, key: str, allow_invalid: bool = False) -> float | None:
    """delivery_ratio は valid 行のみ。RTT は invalid でも値があれば読む(破綻の可視化)。"""
    if not allow_invalid and row.get("valid") != "1":
        return None
    v = row.get(key, "")
    if v in ("", "0", "0.0"):
        return None
    try:
        return float(v)
    except ValueError:
        return None


def grouped_bars(ax, x_labels, libs, values, colors, bar_w=0.25):
    x = np.arange(len(x_labels))
    for i, lib in enumerate(libs):
        offset = (i - (len(libs) - 1) / 2) * bar_w
        ax.bar(x + offset, values[lib], bar_w, label=lib, color=colors[lib])
    ax.set_xticks(x)
    ax.set_xticklabels(x_labels)


def plot_dr_grid():
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.2), sharey=True)
    losses = [1, 5]
    x_labels = ["unrel/1%", "rel/1%", "unrel/5%", "rel/5%"]
    for ax, conns in zip(axes, (50, 200)):
        rows_unrel = {ls: load("sw", ls, conns) for ls in losses}
        rows_rel = {ls: load("rel", ls, conns) for ls in losses}
        values = {lib: [] for lib in LIB_ORDER}
        for ls in losses:
            for src, key in ((rows_unrel, "u"), (rows_rel, "r")):
                for lib in LIB_ORDER:
                    row = src[ls].get(lib)
                    dr = cell(row, "delivery_ratio") if row else None
                    values[lib].append(dr if dr is not None else 0.0)
        grouped_bars(ax, x_labels, LIB_ORDER, values, LIB_COLOR)
        ax.set_title(f"conns={conns}")
        ax.set_ylim(0, 1.1)
        ax.axhline(1.0, color="black", lw=0.5, ls="--")
        ax.grid(True, axis="y", alpha=0.3)
    axes[0].set_ylabel("delivery_ratio")
    axes[0].legend(loc="lower left", framealpha=0.9)
    fig.suptitle("Delivery ratio: unreliable vs reliable (loss × conns)")
    fig.tight_layout()
    fig.savefig(PLOTS / "delivery_ratio.png", dpi=140)
    plt.close(fig)


def plot_p99_grid():
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.2), sharey=True)
    losses = [1, 5]
    x_labels = ["unrel/1%", "rel/1%", "unrel/5%", "rel/5%"]
    for ax, conns in zip(axes, (50, 200)):
        rows_unrel = {ls: load("sw", ls, conns) for ls in losses}
        rows_rel = {ls: load("rel", ls, conns) for ls in losses}
        values = {lib: [] for lib in LIB_ORDER}
        for ls in losses:
            for src, key in ((rows_unrel, "rtt_u_p99_us"), (rows_rel, "rtt_r_p99_us")):
                for lib in LIB_ORDER:
                    row = src[ls].get(lib)
                    v = cell(row, key, allow_invalid=True) if row else None
                    values[lib].append(v / 1000.0 if v else 0.0)  # us -> ms
        grouped_bars(ax, x_labels, LIB_ORDER, values, LIB_COLOR)
        # FAIL bar に印
        for i, lib in enumerate(LIB_ORDER):
            for j, ls in enumerate(losses):
                for k, src in enumerate((rows_unrel, rows_rel)):
                    row = src[ls].get(lib)
                    if row and row.get("valid") == "0":
                        x = j * 2 + k
                        offset = (i - (len(LIB_ORDER) - 1) / 2) * 0.25
                        y = values[lib][x]
                        if y > 0:
                            ax.annotate("✗FAIL", xy=(x + offset, y), ha="center",
                                        va="bottom", fontsize=7, color="red")
        ax.set_title(f"conns={conns}")
        ax.set_yscale("log")
        ax.set_ylim(50, 30000)
        ax.grid(True, axis="y", alpha=0.3, which="both")
    axes[0].set_ylabel("RTT p99 (ms, log)")
    axes[0].legend(loc="upper left", framealpha=0.9)
    fig.suptitle("RTT p99: unreliable vs reliable (retx tail)")
    fig.tight_layout()
    fig.savefig(PLOTS / "rtt_p99.png", dpi=140)
    plt.close(fig)


def plot_unrel_scale():
    """unreliable matrix を conn 軸でスケール感を見せる(reliable は無関係)"""
    fig, axes = plt.subplots(1, 2, figsize=(11, 4.2))
    conns_arr = [10, 50, 100, 200]
    for ax, loss in zip(axes, (1, 5)):
        for lib in LIB_ORDER:
            ys = []
            for cn in conns_arr:
                row = load("sw", loss, cn).get(lib)
                v = cell(row, "rtt_u_p99_us") if row else None
                ys.append(v / 1000.0 if v else None)
            ax.plot(conns_arr, ys, "o-", color=LIB_COLOR[lib], label=lib)
        ax.set_title(f"loss={loss}%")
        ax.set_xlabel("conns")
        ax.set_ylabel("unreliable RTT p99 (ms)")
        ax.set_xscale("log")
        ax.set_ylim(60, 80)
        ax.grid(True, alpha=0.3)
        ax.legend()
    fig.suptitle("Unreliable RTT p99 vs conns (no retransmit)")
    fig.tight_layout()
    fig.savefig(PLOTS / "unrel_scale.png", dpi=140)
    plt.close(fig)


def main() -> None:
    PLOTS.mkdir(exist_ok=True)
    plot_dr_grid()
    plot_p99_grid()
    plot_unrel_scale()
    print("wrote:", sorted(p.name for p in PLOTS.glob("*.png")))


if __name__ == "__main__":
    main()
