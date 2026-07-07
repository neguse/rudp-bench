#!/bin/bash
# sentinel 起動ラッパー。
#
# このリポジトリの rig は CPU 隔離(orchestrator isolate setup)適用中、
# system.slice / user.slice が OS コアに退避されるため、素の shell から
# 起動すると bench コアへの taskset が EINVAL で全プロセス即死する。
# 本スクリプトは bench.slice の transient unit として sentinel を起動し、
# 前回異常終了で残った netns も冪等に掃除する。
#
# usage: sudo scripts/run-sentinel.sh [config.json]
#   config 省略時は orchestrator/examples/sentinel-home.json
set -eu

REPO="$(cd "$(dirname "$0")/.." && pwd)"
CONFIG="${1:-$REPO/orchestrator/examples/sentinel-home.json}"

if [[ $EUID -ne 0 ]]; then
  echo "run-sentinel: root が必要です(sudo で実行)" >&2
  exit 2
fi
if [[ ! -f "$CONFIG" ]]; then
  echo "run-sentinel: config が見つかりません: $CONFIG" >&2
  exit 2
fi

# rig 記述から bench_cpus を読む(config の rig フィールド経由)
RIG_PATH=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['rig'])" "$CONFIG")
[[ "$RIG_PATH" = /* ]] || RIG_PATH="$REPO/$RIG_PATH"
BENCH_CPUS=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['bench_cpus'])" "$RIG_PATH")

# 前回異常終了の netns 残留を冪等に掃除(orchestrator は既存 netns で失敗する)
ip netns del rudpbench-srv 2>/dev/null || true
ip netns del rudpbench-cli 2>/dev/null || true

BIN="$REPO/build/bin/orchestrator"
if [[ ! -x "$BIN" ]]; then
  echo "run-sentinel: $BIN がありません。先に: go build -o build/bin/orchestrator ./orchestrator/cmd/orchestrator" >&2
  exit 2
fi

# unit 名は毎回一意化(前回 unit の残骸と衝突させない)
UNIT="rudp-sentinel-$(date +%s)-$$"

exec systemd-run --wait --pipe --collect \
  --unit="$UNIT" \
  --slice=bench.slice \
  -p AllowedCPUs="$BENCH_CPUS" \
  -p CPUWeight=10000 \
  -p WorkingDirectory="$REPO" \
  "$BIN" sentinel -config "$CONFIG"
