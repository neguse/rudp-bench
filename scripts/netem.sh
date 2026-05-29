#!/usr/bin/env bash
# Apply netem delay/jitter/loss to the loopback interface for bench runs.
#
# loopback delivers each packet through netem twice (send and recv) on the
# same interface, so the configured one-way delay roughly doubles the
# observed RTT (e.g. delay=25ms -> RTT ~50ms).
#
# IMPORTANT: netem's default queue `limit` is 1000 packets. With delay, the
# qdisc holds ~bandwidth-delay-product packets in flight; at high conn/rate
# the BDP exceeds 1000 and netem silently drops the overflow. That overflow
# is NOT part of the intended delay+loss emulation -- it caps delivery_ratio
# as an artifact of the queue size, not the library under test. So we set a
# large explicit `limit` (default 100000 pkts) that is never the bottleneck
# for the tested conn range. Override via the 5th arg if you specifically
# want to study a bounded queue. (lo egress carries BOTH directions, so
# inflight ~= conns * (rate_r+rate_u) * 2 * one_way_delay_seconds.)
#
# Usage:
#   sudo scripts/netem.sh apply <delay_ms> [jitter_ms] [loss_pct] [limit_pkts]
#   sudo scripts/netem.sh clear
#   sudo scripts/netem.sh show
#   sudo scripts/netem.sh probe   # ping ::1 a few times to measure observed RTT

set -euo pipefail

cmd=${1:-}
case "$cmd" in
    apply)
        delay_ms=${2:?delay_ms required}
        jitter_ms=${3:-0}
        loss_pct=${4:-0}
        limit_pkts=${5:-100000}
        tc qdisc del dev lo root 2>/dev/null || true
        # `limit` first so the queue cap is never the artificial bottleneck.
        args="limit ${limit_pkts} delay ${delay_ms}ms"
        if [ "$jitter_ms" != "0" ]; then
            args="$args ${jitter_ms}ms distribution normal"
        fi
        if [ "$loss_pct" != "0" ]; then
            args="$args loss ${loss_pct}%"
        fi
        tc qdisc add dev lo root netem $args
        tc qdisc show dev lo
        ;;
    clear)
        tc qdisc del dev lo root 2>/dev/null || true
        tc qdisc show dev lo
        ;;
    show)
        tc qdisc show dev lo
        ;;
    probe)
        ping -c 5 -i 0.2 -q ::1 || ping -c 5 -i 0.2 -q 127.0.0.1
        ;;
    *)
        cat <<EOF >&2
usage: $0 apply <delay_ms> [jitter_ms] [loss_pct] [limit_pkts]
       $0 clear
       $0 show
       $0 probe                # measure observed loopback RTT
EOF
        exit 2
        ;;
esac
