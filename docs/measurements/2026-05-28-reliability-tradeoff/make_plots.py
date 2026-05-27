#!/usr/bin/env python3
"""Render reliability tradeoff plots from the matched unreliable/reliable runs.

Inputs are the CSVs under ./data/ (copied from results/). Outputs PNGs under
./plots/. Run from this directory: `python3 make_plots.py`.

Data sources:
  - sw_l{1,5}_{10,50,100,200}.csv         unreliable, N=4, raw_udp/mini_rudp/enet
  - rel_l{1,5}_{50,200}.csv               reliable,   N=4, raw_udp/mini_rudp/enet
  - unrel_n1_l{1,5}_{50,200}.csv          unreliable, N=1, gns/msquic
  - rel_n1_l{1,5}_{50,200}.csv            reliable,   N=1, gns/msquic
"""
from __future__ import annotations

import csv
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np


HERE = Path(__file__).resolve().parent
DATA = HERE / "data"
PLOTS = HERE / "plots"

LIB_ORDER = ["raw_udp", "mini_rudp", "enet", "gns", "msquic"]
LIB_COLOR = {
    "raw_udp":   "#888888",
    "mini_rudp": "#1f77b4",
    "enet":      "#d62728",
    "gns":       "#2ca02c",
    "msquic":    "#ff7f0e",
}


def load(path: Path) -> dict[str, dict]:
    out: dict[str, dict] = {}
    if not path.exists():
        return out
    with path.open() as f:
        for row in csv.DictReader(f):
            out[row["library"]] = row
    return out


def load_unrel(loss: int, conns: int) -> dict[str, dict]:
    rows = load(DATA / f"sw_l{loss}_{conns}.csv")
    rows.update(load(DATA / f"unrel_n1_l{loss}_{conns}.csv"))
    return rows


def load_rel(loss: int, conns: int) -> dict[str, dict]:
    rows = load(DATA / f"rel_l{loss}_{conns}.csv")
    rows.update(load(DATA / f"rel_n1_l{loss}_{conns}.csv"))
    return rows


def load_mix(loss: int, conns: int) -> dict[str, dict]:
    rows = load(DATA / f"mix_l{loss}_{conns}.csv")
    rows.update(load(DATA / f"mix_n1_l{loss}_{conns}.csv"))
    return rows


def cell(row: dict, key: str, allow_invalid: bool = False) -> float | None:
    if row is None:
        return None
    if not allow_invalid and row.get("valid") != "1":
        return None
    v = row.get(key, "")
    if v in ("", "0", "0.0"):
        return None
    try:
        return float(v)
    except ValueError:
        return None


def grouped_bars(ax, x_labels, libs, values, colors, bar_w=0.16):
    x = np.arange(len(x_labels))
    for i, lib in enumerate(libs):
        offset = (i - (len(libs) - 1) / 2) * bar_w
        ax.bar(x + offset, values[lib], bar_w, label=lib, color=colors[lib])
    ax.set_xticks(x)
    ax.set_xticklabels(x_labels)


def annotate_failures(ax, rows_by_x, libs, bar_w=0.16):
    """Mark genuine failures (crash / tick FAIL). unsupported は無印で skip。"""
    for x_idx, src in enumerate(rows_by_x):
        for i, lib in enumerate(libs):
            row = src.get(lib)
            if row is None or row.get("valid") != "0":
                continue
            reason = row.get("invalid_reason", "")
            if "unsupported" in reason:
                continue  # 構造的非対応はマーク不要
            offset = (i - (len(libs) - 1) / 2) * bar_w
            short = "crash" if "crash" in reason else "FAIL"
            ax.annotate(f"✗{short}", xy=(x_idx + offset, ax.get_ylim()[0] * 1.1),
                        ha="center", va="bottom", fontsize=6, color="red", rotation=90)


def plot_dr_grid():
    fig, axes = plt.subplots(1, 2, figsize=(12, 4.5), sharey=True)
    losses = [1, 5]
    x_labels = ["unrel/1%", "rel/1%", "unrel/5%", "rel/5%"]
    for ax, conns in zip(axes, (50, 200)):
        rows_unrel = {ls: load_unrel(ls, conns) for ls in losses}
        rows_rel = {ls: load_rel(ls, conns) for ls in losses}
        values = {lib: [] for lib in LIB_ORDER}
        rows_by_x = []
        for ls in losses:
            for src in (rows_unrel[ls], rows_rel[ls]):
                rows_by_x.append(src)
                for lib in LIB_ORDER:
                    row = src.get(lib)
                    dr = cell(row, "delivery_ratio")
                    values[lib].append(dr if dr is not None else 0.0)
        grouped_bars(ax, x_labels, LIB_ORDER, values, LIB_COLOR)
        ax.set_title(f"conns={conns}")
        ax.set_ylim(0, 1.15)
        ax.axhline(1.0, color="black", lw=0.5, ls="--")
        ax.grid(True, axis="y", alpha=0.3)
        annotate_failures(ax, rows_by_x, LIB_ORDER)
    axes[0].set_ylabel("delivery_ratio")
    axes[0].legend(loc="lower right", framealpha=0.9, ncol=2, fontsize=8)
    fig.suptitle("Delivery ratio: unreliable vs reliable (loss × conns)")
    fig.tight_layout()
    fig.savefig(PLOTS / "delivery_ratio.png", dpi=140)
    plt.close(fig)


def plot_p99_grid():
    fig, axes = plt.subplots(1, 2, figsize=(12, 4.5), sharey=True)
    losses = [1, 5]
    x_labels = ["unrel/1%", "rel/1%", "unrel/5%", "rel/5%"]
    for ax, conns in zip(axes, (50, 200)):
        rows_unrel = {ls: load_unrel(ls, conns) for ls in losses}
        rows_rel = {ls: load_rel(ls, conns) for ls in losses}
        values = {lib: [] for lib in LIB_ORDER}
        rows_by_x = []
        for ls in losses:
            for src, key in ((rows_unrel[ls], "rtt_u_p99_us"),
                             (rows_rel[ls], "rtt_r_p99_us")):
                rows_by_x.append(src)
                for lib in LIB_ORDER:
                    row = src.get(lib)
                    v = cell(row, key, allow_invalid=True)
                    values[lib].append(v / 1000.0 if v else 0.0)
        grouped_bars(ax, x_labels, LIB_ORDER, values, LIB_COLOR)
        ax.set_title(f"conns={conns}")
        ax.set_yscale("log")
        ax.set_ylim(50, 30000)
        ax.grid(True, axis="y", alpha=0.3, which="both")
        # FAIL/crash markers (unsupported は skip)
        for x_idx, src in enumerate(rows_by_x):
            for i, lib in enumerate(LIB_ORDER):
                row = src.get(lib)
                offset = (i - (len(LIB_ORDER) - 1) / 2) * 0.16
                if row is None or row.get("valid") != "0":
                    continue
                reason = row.get("invalid_reason", "")
                if "unsupported" in reason:
                    continue
                y = values[lib][x_idx]
                short = "crash" if "crash" in reason else "FAIL"
                if y > 0:
                    ax.annotate(f"✗{short}", xy=(x_idx + offset, y),
                                ha="center", va="bottom", fontsize=7, color="red")
                else:
                    ax.annotate(f"✗{short}", xy=(x_idx + offset, 55),
                                ha="center", va="bottom", fontsize=6,
                                color="red", rotation=90)
    axes[0].set_ylabel("RTT p99 (ms, log)")
    axes[0].legend(loc="upper left", framealpha=0.9, ncol=2, fontsize=8)
    fig.suptitle("RTT p99: unreliable vs reliable (retx tail)")
    fig.tight_layout()
    fig.savefig(PLOTS / "rtt_p99.png", dpi=140)
    plt.close(fig)


def plot_unrel_scale():
    """unreliable RTT vs conns. gns/msquic は 50/200 のみ点で。"""
    fig, axes = plt.subplots(1, 2, figsize=(12, 4.5))
    conns_full = [10, 50, 100, 200]
    conns_partial = [50, 200]
    for ax, loss in zip(axes, (1, 5)):
        for lib in ["raw_udp", "mini_rudp", "enet"]:
            ys = []
            for cn in conns_full:
                row = load_unrel(loss, cn).get(lib)
                v = cell(row, "rtt_u_p99_us", allow_invalid=True)
                ys.append(v / 1000.0 if v else None)
            ax.plot(conns_full, ys, "o-", color=LIB_COLOR[lib], label=lib)
        for lib in ["gns", "msquic"]:
            xs, ys = [], []
            for cn in conns_partial:
                row = load_unrel(loss, cn).get(lib)
                v = cell(row, "rtt_u_p99_us", allow_invalid=False)
                if v:
                    xs.append(cn)
                    ys.append(v / 1000.0)
            ax.plot(xs, ys, "s--", color=LIB_COLOR[lib], label=lib + " (N=1)")
        ax.set_title(f"loss={loss}%")
        ax.set_xlabel("conns")
        ax.set_ylabel("unreliable RTT p99 (ms)")
        ax.set_xscale("log")
        ax.set_ylim(60, 80)
        ax.grid(True, alpha=0.3)
        ax.legend(fontsize=8)
    fig.suptitle("Unreliable RTT p99 vs conns (no retransmit)")
    fig.tight_layout()
    fig.savefig(PLOTS / "unrel_scale.png", dpi=140)
    plt.close(fig)


def plot_hol():
    """HoL leakage: mix の unreliable u99 を unrel-only u99 と比較."""
    fig, axes = plt.subplots(1, 2, figsize=(12, 4.5), sharey=True)
    # mini_rudp, enet を共通 lib として、gns/msquic も追加
    hol_libs = ["mini_rudp", "enet", "gns", "msquic"]
    losses = [1, 5]
    x_labels = ["unrel-only/1%", "mix/1%", "unrel-only/5%", "mix/5%"]
    for ax, conns in zip(axes, (50, 200)):
        rows_unrel = {ls: load_unrel(ls, conns) for ls in losses}
        rows_mix = {ls: load_mix(ls, conns) for ls in losses}
        values = {lib: [] for lib in hol_libs}
        rows_by_x = []
        for ls in losses:
            for src in (rows_unrel[ls], rows_mix[ls]):
                rows_by_x.append(src)
                for lib in hol_libs:
                    row = src.get(lib)
                    v = cell(row, "rtt_u_p99_us", allow_invalid=True)
                    values[lib].append(v / 1000.0 if v else 0.0)
        colors = {lib: LIB_COLOR[lib] for lib in hol_libs}
        grouped_bars(ax, x_labels, hol_libs, values, colors, bar_w=0.2)
        ax.set_title(f"conns={conns}")
        ax.set_yscale("log")
        ax.set_ylim(50, 5000)
        ax.grid(True, axis="y", alpha=0.3, which="both")
        ax.axhline(67, color="black", lw=0.5, ls=":", label="baseline 67ms")
    axes[0].set_ylabel("unreliable RTT p99 (ms, log)")
    axes[0].legend(loc="upper left", framealpha=0.9, ncol=2, fontsize=8)
    fig.suptitle("HoL leakage: unreliable RTT p99 with vs without reliable traffic")
    fig.tight_layout()
    fig.savefig(PLOTS / "hol_leakage.png", dpi=140)
    plt.close(fig)


def main() -> None:
    PLOTS.mkdir(exist_ok=True)
    plot_dr_grid()
    plot_p99_grid()
    plot_unrel_scale()
    plot_hol()
    print("wrote:", sorted(p.name for p in PLOTS.glob("*.png")))


if __name__ == "__main__":
    main()
