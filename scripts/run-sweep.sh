#!/bin/bash
# sweep 起動ラッパー(run-sentinel.sh と同型)。
#
# rig は CPU 隔離適用中で system.slice / user.slice が OS コアに退避される
# ため、素の shell から起動すると bench コアへの taskset が EINVAL で全
# プロセス即死する。本スクリプトは bench.slice の transient unit として
# sweep を起動し、残留 netns も冪等に掃除する。
#
# usage: sudo scripts/run-sweep.sh <sweep-config.json> [rig.json]
#   rig 省略時は orchestrator/rigs/home.json
set -eu

REPO="$(cd "$(dirname "$0")/.." && pwd)"
CONFIG="${1:?usage: run-sweep.sh <sweep-config.json> [rig.json]}"
RIG_PATH="${2:-$REPO/orchestrator/rigs/home.json}"

if [[ $EUID -ne 0 ]]; then
  echo "run-sweep: root が必要です(sudo で実行)" >&2
  exit 2
fi
if [[ ! -f "$CONFIG" ]]; then
  echo "run-sweep: config が見つかりません: $CONFIG" >&2
  exit 2
fi

BENCH_CPUS=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['bench_cpus'])" "$RIG_PATH")

# 前回異常終了の netns 残留を冪等に掃除(orchestrator は既存 netns で失敗する)
ip netns del rudpbench-srv 2>/dev/null || true
ip netns del rudpbench-cli 2>/dev/null || true

BIN="$REPO/build/bin/orchestrator"
if [[ ! -x "$BIN" ]]; then
  echo "run-sweep: $BIN がありません。先に: go build -o build/bin/orchestrator ./orchestrator/cmd/orchestrator" >&2
  exit 2
fi

UNIT="rudp-sweep-$(date +%s)-$$"

exec systemd-run --wait --pipe --collect \
  --unit="$UNIT" \
  --slice=bench.slice \
  -p AllowedCPUs="$BENCH_CPUS" \
  -p CPUWeight=10000 \
  -p WorkingDirectory="$REPO" \
  "$BIN" sweep -config "$CONFIG"
