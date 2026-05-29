#!/usr/bin/env bash
# Usage:
#   sudo scripts/set_loss.sh apply <pct> [limit_pkts]   # netem loss を lo に適用 (例: 5)
#   sudo scripts/set_loss.sh clear                      # qdisc を削除
#
# netem の既定 queue limit は 1000 pkt で、高 conn/rate ではキュー溢れが
# delivery を頭打ちにするアーティファクトになる。明示的に大きな limit を張る。
set -euo pipefail

cmd=${1:-}
case "$cmd" in
  apply)
    pct=${2:-0}
    limit_pkts=${3:-100000}
    tc qdisc del dev lo root 2>/dev/null || true
    if [ "$pct" != "0" ]; then
      tc qdisc add dev lo root netem limit "${limit_pkts}" loss "${pct}%"
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
