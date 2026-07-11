#!/usr/bin/env bash
# 校正: duration 不変性
#
# 同一条件で duration だけ変えて2回走らせ、delivery が一致することを検査する。
# 「delivery が duration 非依存の一定割合で欠ける」= 計測窓ズレ系バグの検出器
# (v1 dev-notes §1.7 の教訓)。loopback・netem なし・sudo 不要。
#
# 環境変数: DUR_SHORT / DUR_LONG(既定 4s / 12s。手元での本式は 10s / 30s)
#             CALIBRATION_DIR(指定時はshort/longのrun bundleとsummary.jsonを保存)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DUR_SHORT="${DUR_SHORT:-4s}"
DUR_LONG="${DUR_LONG:-12s}"
TOL="${TOL:-0.005}"
CALIBRATION_DIR="${CALIBRATION_DIR:-}"
if [[ -n "$CALIBRATION_DIR" ]]; then
  if [[ -e "$CALIBRATION_DIR" ]]; then
    printf 'calibration output already exists: %s\n' "$CALIBRATION_DIR" >&2
    exit 2
  fi
  mkdir -p "$CALIBRATION_DIR"
  WORK="$CALIBRATION_DIR"
else
  WORK="$(mktemp -d)"
  trap 'rm -rf "$WORK"' EXIT
fi

# ビルド(既にあれば no-op)
cmake -S "$ROOT/servers/enet" -B "$ROOT/build-v2-enet" >/dev/null
cmake --build "$ROOT/build-v2-enet" -j >/dev/null
go build -o "$ROOT/build-v2-go/orchestrator" "$ROOT/orchestrator/cmd/orchestrator" >/dev/null

run_once() {
  local duration="$1" out="$2" port="$3"
  mkdir -p "$out"
  cat > "$out/config.json" <<EOF
{
  "transport": "enet",
  "server_command": ["$ROOT/build-v2-enet/enet_server", "--port", "$port"],
  "client_command": ["$ROOT/build-v2-enet/enet_client",
    "--host", "127.0.0.1", "--port", "$port",
    "--conns", "{conns}", "--proc-index", "{proc_index}",
    "--origin-base", "{origin_id_start}",
    "--rate-lt", "50", "--rate-md", "50", "--payload", "64",
    "--deadline-ns", "150000000", "--staleness-period-ns", "10000000"],
  "client_procs": 2,
  "total_conns": 4,
  "warmup": "1s",
  "duration": "$duration",
  "drain": "1s",
  "deadline_ns": 150000000,
  "staleness_period_ns": 10000000,
  "output_dir": "$out"
}
EOF
  "$ROOT/build-v2-go/orchestrator" run -config "$out/config.json" >/dev/null
}

run_once "$DUR_SHORT" "$WORK/short" 42911
run_once "$DUR_LONG" "$WORK/long" 42913

python3 - "$WORK/short/result.json" "$WORK/long/result.json" "$TOL" "$WORK/summary.json" <<'PY'
import datetime
import json
import sys

short = json.load(open(sys.argv[1]))
long_ = json.load(open(sys.argv[2]))
tol = float(sys.argv[3])
summary_path = sys.argv[4]

ok = True
summary = {
    "version": 1,
    "kind": "duration_invariance",
    "generated_at": datetime.datetime.now(datetime.timezone.utc).isoformat(),
    "tolerance": tol,
    "short_result": "short/result.json",
    "long_result": "long/result.json",
    "classes": {},
}
for name, r in (("short", short), ("long", long_)):
    if r["verdict"] != "VALID":
        print(f"FAIL: {name} run verdict={r['verdict']}")
        ok = False

for cls in ("loss_tolerant", "must_deliver"):
    short_cls = short["metrics"]["classes"][cls]
    long_cls = long_["metrics"]["classes"][cls]
    for label, metrics in (("short", short_cls), ("long", long_cls)):
        if metrics["slots"] <= 0 or metrics["submitted"] <= 0 or metrics["expected_receives"] <= 0 or metrics["delivered_unique"] <= 0:
            print(f"FAIL: {label} {cls} has empty accounting: {metrics}")
            ok = False
        if metrics["submitted"] != metrics["slots"]:
            print(f"FAIL: {label} {cls} submitted={metrics['submitted']} slots={metrics['slots']}")
            ok = False
    ds = short_cls["delivery_ratio"]
    dl = long_cls["delivery_ratio"]
    diff = abs(ds - dl)
    status = "ok" if diff <= tol else "FAIL"
    print(f"{status}: {cls} delivery short={ds:.4f} long={dl:.4f} |diff|={diff:.4f} (tol={tol})")
    if diff > tol:
        ok = False
    short_window = (short["control"]["schedule"]["stop_at_ns"] - short["control"]["schedule"]["start_at_ns"]) / 1e9
    long_window = (long_["control"]["schedule"]["stop_at_ns"] - long_["control"]["schedule"]["start_at_ns"]) / 1e9
    short_rate = short_cls["slots"] / short_window
    long_rate = long_cls["slots"] / long_window
    rate_diff = abs(short_rate - long_rate) / max(short_rate, long_rate)
    print(f"{'ok' if rate_diff <= 0.01 else 'FAIL'}: {cls} offered slot rate short={short_rate:.2f}/s long={long_rate:.2f}/s")
    if rate_diff > 0.01:
        ok = False

    summary["classes"][cls] = {
        "short": {
            "window_s": short_window,
            "slots": short_cls["slots"],
            "submitted": short_cls["submitted"],
            "expected_receives": short_cls["expected_receives"],
            "delivered_unique": short_cls["delivered_unique"],
            "delivery_ratio": ds,
            "offered_slot_rate_hz": short_rate,
        },
        "long": {
            "window_s": long_window,
            "slots": long_cls["slots"],
            "submitted": long_cls["submitted"],
            "expected_receives": long_cls["expected_receives"],
            "delivered_unique": long_cls["delivered_unique"],
            "delivery_ratio": dl,
            "offered_slot_rate_hz": long_rate,
        },
        "delivery_ratio_absolute_difference": diff,
        "offered_rate_relative_difference": rate_diff,
    }

summary["passed"] = ok
with open(summary_path, "w") as f:
    json.dump(summary, f, indent=2)
    f.write("\n")

sys.exit(0 if ok else 1)
PY
echo "duration invariance: PASS"
