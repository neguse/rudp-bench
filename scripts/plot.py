#!/usr/bin/env python3
"""Phase 1 / Phase 2 result post-processing.

Usage:
    plot.py phase1-table --in results/phase1.csv --out results/phase1_table.md
    plot.py phase2-plot  --in results/phase2/<axis>.csv --out results/phase2/plots/ --axis loss
"""
import argparse
import sys
from pathlib import Path

import pandas as pd


def phase1_table(args: argparse.Namespace) -> int:
    df = pd.read_csv(args.in_path)
    df["scenario"] = (
        df["reliable"].astype(str)
        + "/" + df["size"].astype(str)
        + "/" + df["conns"].astype(str)
        + "/" + df["rate"].astype(str)
        + "/" + df["loss"].astype(str).str.rstrip("0").str.rstrip(".")
    )
    pivot_throughput = df.pivot_table(
        index="scenario", columns="library", values="throughput_mbps", aggfunc="mean"
    )
    pivot_delivery = df.pivot_table(
        index="scenario", columns="library", values="delivery_ratio", aggfunc="mean"
    )
    out = Path(args.out_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    with out.open("w") as f:
        f.write("# Phase 1 results\n\n")
        f.write("## throughput (Mbps)\n\n")
        f.write(pivot_throughput.to_markdown())
        f.write("\n\n## delivery_ratio\n\n")
        f.write(pivot_delivery.to_markdown())
        f.write("\n")
    print(f"wrote {out}")
    return 0


def phase2_plot(args: argparse.Namespace) -> int:
    import matplotlib.pyplot as plt
    df = pd.read_csv(args.in_path)
    out_dir = Path(args.out_path)
    out_dir.mkdir(parents=True, exist_ok=True)
    axis_col = args.axis  # 例: "loss", "rate", "size", "conns"
    for metric in ["throughput_mbps", "delivery_ratio", "rtt_p50_us"]:
        fig, ax = plt.subplots(figsize=(8, 5))
        for lib, sub in df.groupby("library"):
            sub = sub.sort_values(axis_col)
            ax.plot(sub[axis_col], sub[metric], marker="o", label=lib)
        ax.set_xlabel(axis_col)
        ax.set_ylabel(metric)
        ax.set_title(f"{metric} vs {axis_col}")
        ax.legend()
        ax.grid(True, alpha=0.3)
        path = out_dir / f"{axis_col}_{metric}.png"
        fig.savefig(path, dpi=120, bbox_inches="tight")
        plt.close(fig)
        print(f"wrote {path}")
    return 0


def main() -> int:
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    p1 = sub.add_parser("phase1-table")
    p1.add_argument("--in", dest="in_path", required=True)
    p1.add_argument("--out", dest="out_path", required=True)
    p1.set_defaults(func=phase1_table)
    p2 = sub.add_parser("phase2-plot")
    p2.add_argument("--in", dest="in_path", required=True)
    p2.add_argument("--out", dest="out_path", required=True)
    p2.add_argument("--axis", required=True, choices=["loss", "rate", "size", "conns"])
    p2.set_defaults(func=phase2_plot)
    args = p.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
