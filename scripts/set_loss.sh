#!/usr/bin/env bash
# Usage:
#   sudo scripts/set_loss.sh apply <pct>   # netem loss を lo に適用 (例: 5)
#   sudo scripts/set_loss.sh clear         # qdisc を削除
set -euo pipefail

cmd=${1:-}
case "$cmd" in
  apply)
    pct=${2:-0}
    tc qdisc del dev lo root 2>/dev/null || true
    if [ "$pct" != "0" ]; then
      tc qdisc add dev lo root netem loss "${pct}%"
    fi
    tc qdisc show dev lo
    ;;
  clear)
    tc qdisc del dev lo root 2>/dev/null || true
    tc qdisc show dev lo
    ;;
  *)
    echo "usage: $0 apply <pct> | clear" >&2
    exit 2
    ;;
esac
