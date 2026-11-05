#!/usr/bin/env bash
# 劣悪ベンチの run config 群を順に実行する。ゲームサーバ等と CPU を分け合わないよう、
# bench.slice(rig の bench_cpus)の中で root として動かし、SUT は SUDO_UID へ権限を落とす。
# usage: sudo scripts/badnet/run.sh <orchestrator> <config.json>...
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
RIG_CONFIG="$ROOT/orchestrator/rigs/home.json"

if [[ $EUID -ne 0 || -z "${SUDO_UID:-}" ]]; then
  echo 'badnet/run.sh: run through sudo' >&2
  exit 2
fi
if [[ "${RUDP_BADNET_SCOPE:-0}" != 1 ]]; then
  INVOKER_HOME="$(getent passwd "$SUDO_UID" | cut -d: -f6)"
  exec systemd-run --wait --pipe --collect --quiet \
    --unit="rudp-badnet-$(date +%s)-$$" \
    --slice=bench.slice \
    -p AllowedCPUs="$(jq -er '.bench_cpus' "$RIG_CONFIG")" \
    -p CPUWeight=10000 \
    -p WorkingDirectory="$PWD" \
    /usr/bin/env RUDP_BADNET_SCOPE=1 \
    SUDO_UID="$SUDO_UID" SUDO_GID="$SUDO_GID" \
    SUDO_USER="$(id -nu "$SUDO_UID")" USER="$(id -nu "$SUDO_UID")" LOGNAME="$(id -nu "$SUDO_UID")" \
    HOME="$INVOKER_HOME" DOTNET_ROOT="$INVOKER_HOME/.dotnet" \
    /bin/bash "$0" "$@"
fi

ORCHESTRATOR="$1"
shift
cleanup_netns() {
  ip netns del rudpbench-srv 2>/dev/null || true
  ip netns del rudpbench-cli 2>/dev/null || true
}
trap cleanup_netns EXIT

for cfg in "$@"; do
  cleanup_netns
  start=$(date +%s)
  rc=0
  "$ORCHESTRATOR" run -config "$cfg" >"${cfg%.json}.log" 2>&1 || rc=$?
  printf '%s rc=%d %ds\n' "$(basename "$cfg")" "$rc" "$(( $(date +%s) - start ))"
done
