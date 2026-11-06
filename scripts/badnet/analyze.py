#!/usr/bin/env python3
"""劣悪ベンチの result.json 群を判定し、markdown 表と JSON を出す。判定規則の正は README の「劣悪ベンチ」。"""
import argparse
import glob
import json
import os

S = 1_000_000_000
MS = 1_000_000


def bin_upper_ns(i, n_bins):
    # benchkit/src/metrics.c hist_bin_upper_ns と同じ log2x16 レイアウト
    if i == 0:
        return 1000
    if i >= n_bins - 1:
        return 100 * S
    major, sub = divmod(i, 16)
    low = 1000 << major
    return min(low + (low * (sub + 1) + 15) // 16, 100 * S)


def hist_max_ns(h):
    bins = h.get("bins") or []
    for i in range(len(bins) - 1, -1, -1):
        if bins[i]:
            return bin_upper_ns(i, len(bins))
    return 0


def egress_loss(e):
    return (e or {}).get("loss_pct", 0) / 100.0


def classify(cfg):
    if cfg.get("netem_schedule"):
        return "disturbance"
    if cfg["netem"]["server_egress"].get("rate"):
        return "congestion"
    return "steady"


def parse_dur(s):
    s = s.strip()
    if s.endswith("ms"):
        return float(s[:-2]) / 1000
    if s.endswith("s"):
        return float(s[:-1])
    raise ValueError(s)


def judge(result):
    cfg = result["config"]
    kind = classify(cfg)
    window_s = parse_dur(cfg["duration"]) if isinstance(cfg["duration"], str) else cfg["duration"] / S
    out = {"transport": result["transport"], "verdict": result.get("verdict"),
           "outcome": result.get("outcome"), "kind": kind, "fails": [], "dirs": {}}
    if result.get("verdict") != "VALID":
        out["fails"].append("INVALID: " + "; ".join(result.get("invalid_reasons") or []))
        return out

    # C1: 切断・異常終了なし
    for p in result.get("processes", []):
        if p.get("exit_code") != 0:
            out["fails"].append(f"C1 {p['role']} exit={p.get('exit_code')}")
    for part in (result.get("control") or {}).get("participants", []):
        stats = (part.get("done") or {}).get("stats") or {}
        if stats.get("peer_disconnects"):
            out["fails"].append(f"C1 peer_disconnects={stats['peer_disconnects']}")

    sched = cfg.get("netem_schedule") or []
    x_s, p_bad = 0.0, 0.0
    if sched:
        x_s = parse_dur(sched[1]["at"]) - parse_dur(sched[0]["at"])
        p_bad = egress_loss(sched[0]["server_egress"])

    for t in (result.get("metrics") or {}).get("traffic", []):
        d = "up" if t["direction"] == "client_to_server" else "down"
        egress = cfg["netem"]["client_egress" if d == "up" else "server_egress"]
        p = egress_loss(egress)
        r = out["dirs"].setdefault(d, {})
        if t["class"] == "loss_tolerant":
            r["lt_delivery"] = t["delivery_ratio"]
            r["lt_stale_p99_ms"] = t["staleness_ns"]["p99_ns"] / MS
            r["lt_stale_max_ms"] = hist_max_ns(t["staleness_ns"]) / MS
            if kind == "steady":
                need = 0.9 * (1 - p)
                if r["lt_delivery"] < need:
                    out["fails"].append(f"C3 {d} LT delivery {r['lt_delivery']:.3f}<{need:.3f}")
                if r["lt_stale_p99_ms"] > 500:
                    out["fails"].append(f"C3 {d} LT staleness p99 {r['lt_stale_p99_ms']:.0f}ms>500")
            elif kind == "congestion":
                if r["lt_stale_p99_ms"] > 500:
                    out["fails"].append(f"C3 {d} LT staleness p99 {r['lt_stale_p99_ms']:.0f}ms>500")
            else:
                expected = ((window_s - x_s) + x_s * (1 - p_bad)) / window_s
                need = 0.9 * expected
                if r["lt_delivery"] < need:
                    out["fails"].append(f"C4 {d} LT delivery {r['lt_delivery']:.3f}<{need:.3f}")
                if r["lt_stale_max_ms"] > (x_s + 1) * 1000:
                    out["fails"].append(f"C4 {d} LT staleness max {r['lt_stale_max_ms']:.0f}ms>{(x_s + 1) * 1000:.0f}")
        else:
            r["md_delivery"] = t["delivery_ratio"]
            r["md_p99_ms"] = t["latency_sched_ns"]["p99_ns"] / MS
            r["md_max_ms"] = hist_max_ns(t["latency_sched_ns"]) / MS
            if r["md_delivery"] < 1.0:
                out["fails"].append(f"C2 {d} MD delivery {r['md_delivery']:.4f}<1")
            if kind == "steady" and r["md_p99_ms"] > 1000:
                out["fails"].append(f"C2 {d} MD p99 {r['md_p99_ms']:.0f}ms>1000")
            if kind == "congestion" and r["md_p99_ms"] > 500:
                out["fails"].append(f"C3 {d} MD p99 {r['md_p99_ms']:.0f}ms>500")
            if kind == "disturbance" and r["md_max_ms"] > (x_s + 1) * 1000:
                out["fails"].append(f"C4 {d} MD max {r['md_max_ms']:.0f}ms>{(x_s + 1) * 1000:.0f}")
    if not out["dirs"]:
        out["fails"].append("no metrics")
    return out


def cell(j):
    if j["fails"] and j["fails"][0].startswith("INVALID"):
        return "INVALID"
    codes = sorted({f.split()[0] for f in j["fails"]})
    return "PASS" if not codes else "FAIL " + "/".join(codes)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("session")
    args = ap.parse_args()
    rows = {}
    for path in sorted(glob.glob(os.path.join(args.session, "*", "*", "result.json"))):
        cond = path.split(os.sep)[-3]
        with open(path) as f:
            j = judge(json.load(f))
        rows.setdefault(cond, {})[j["transport"]] = j
    transports = sorted({t for r in rows.values() for t in r})
    with open(os.path.join(args.session, "judgement.json"), "w") as f:
        json.dump(rows, f, indent=2)
    print("| condition | " + " | ".join(transports) + " |")
    print("|---|" + "---|" * len(transports))
    for cond, r in rows.items():
        print(f"| {cond} | " + " | ".join(cell(r[t]) if t in r else "-" for t in transports) + " |")


if __name__ == "__main__":
    main()
