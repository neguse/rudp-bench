#!/usr/bin/env python3
"""Publish a canonical benchmark run to the stable docs/measurements/current path."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REQUIRED_FILES = [
    "capacity.csv",
    "summary.csv",
    "results_all.csv",
    "scenarios_all.csv",
    "profiles.csv",
]


def rel(path: Path) -> str:
    try:
        return path.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return str(path)


def copy_run_files(run_dir: Path, dest: Path) -> None:
    missing = [name for name in REQUIRED_FILES if not (run_dir / name).exists()]
    if missing:
        raise FileNotFoundError(f"{run_dir} is missing: {', '.join(missing)}")

    dest.mkdir(parents=True, exist_ok=True)
    for name in REQUIRED_FILES:
        src = run_dir / name
        dst = dest / name
        text = src.read_text(encoding="utf-8").replace("\r\n", "\n").replace("\r", "\n")
        with dst.open("w", encoding="utf-8", newline="\n") as f:
            f.write(text)

    plots = dest / "plots"
    if plots.exists():
        shutil.rmtree(plots)
    report = dest / "report.md"
    if report.exists():
        report.unlink()


def render_report(dest: Path, source_run: Path) -> None:
    label = f"{rel(dest)} (published from {rel(source_run)})"
    subprocess.run(
        [
            sys.executable,
            str(ROOT / "scripts/render_canonical_report.py"),
            "--run-dir",
            str(dest),
            "--run-label",
            label,
        ],
        check=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", required=True, help="Canonical run directory under results/")
    parser.add_argument("--dest", default=str(ROOT / "docs/measurements/current"))
    args = parser.parse_args()

    run_dir = Path(args.run_dir).resolve()
    dest = Path(args.dest).resolve()
    copy_run_files(run_dir, dest)
    render_report(dest, run_dir)
    print(f"published canonical result to {dest}")
    print(f"current report: {dest / 'report.md'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
