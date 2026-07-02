#!/usr/bin/env bash
# 校正: duration 不変性
#
# 同一条件で duration だけ変えて2回走らせ、delivery が一致することを検査する。
# 「delivery が duration 非依存の一定割合で欠ける」= 計測窓ズレ系バグの検出器
# (v1 dev-notes §1.7 の教訓)。loopback・netem なし・sudo 不要。
#
# 環境変数: DUR_SHORT / DUR_LONG(既定 4s / 12s。手元での本式は 10s / 30s)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DUR_SHORT="${DUR_SHORT:-4s}"
DUR_LONG="${DUR_LONG:-12s}"
TOL="${TOL:-0.005}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

# ビルド(既にあれば no-op)
cmake -S "$ROOT/servers/enet" -B "$ROOT/build-v2-enet" >/dev/null
cmake --build "$ROOT/build-v2-enet" -j >/dev/null
go build -o "$WORK/orchestrator" "$ROOT/orchestrator/cmd/orchestrator" >/dev/null

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
  "$WORK/orchestrator" run -config "$out/config.json" >/dev/null
}

run_once "$DUR_SHORT" "$WORK/short" 42911
run_once "$DUR_LONG" "$WORK/long" 42913

python3 - "$WORK/short/result.json" "$WORK/long/result.json" "$TOL" <<'PY'
import json, sys

short = json.load(open(sys.argv[1]))
long_ = json.load(open(sys.argv[2]))
tol = float(sys.argv[3])

ok = True
for name, r in (("short", short), ("long", long_)):
    if r["verdict"] != "VALID":
        print(f"FAIL: {name} run verdict={r['verdict']}")
        ok = False

for cls in ("loss_tolerant", "must_deliver"):
    ds = short["metrics"]["classes"][cls]["delivery_ratio"]
    dl = long_["metrics"]["classes"][cls]["delivery_ratio"]
    diff = abs(ds - dl)
    status = "ok" if diff <= tol else "FAIL"
    print(f"{status}: {cls} delivery short={ds:.4f} long={dl:.4f} |diff|={diff:.4f} (tol={tol})")
    if diff > tol:
        ok = False

sys.exit(0 if ok else 1)
PY
echo "duration invariance: PASS"
