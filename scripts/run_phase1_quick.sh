#!/usr/bin/env bash
# Phase 1 quick sweep — axis を 1 シナリオに固定して 10 lib 一巡(~5min)。
# 軸: size=100, conns=10, mode=echo, reliable=r, rate=50, loss=0,
#     duration=20, warmup=2 (litenetlib のみ 5)
#
# Usage:
#   scripts/run_phase1_quick.sh [--libraries=raw_udp,...] [--build-dir=build] [--results=results/phase1_quick.csv]

set -euo pipefail

LIBS="raw_udp,mini_rudp,enet,kcp,slikenet,udt4,yojimbo,gns,litenetlib,msquic"
BUILD_DIR="build"
RESULTS="results/phase1_quick.csv"

for arg in "$@"; do
  case "$arg" in
    --libraries=*) LIBS="${arg#*=}" ;;
    --build-dir=*) BUILD_DIR="${arg#*=}" ;;
    --results=*) RESULTS="${arg#*=}" ;;
    *) echo "unknown arg: $arg" >&2; exit 2 ;;
  esac
done

BIN="$BUILD_DIR/harness/rudp-bench"
[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 2; }

LITENETLIB_BIN="adapters/litenetlib/bin/Release/net10.0/litenetlib_adapter"
LITENETLIB_WARMUP=5

mkdir -p "$(dirname "$RESULTS")"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

echo "library,encryption,phase,reliable,size,conns,rate,loss,throughput_mbps,msg_per_sec,rtt_p50_us,rtt_p95_us,rtt_p99_us,delivered,sent,delivery_ratio,cpu_pct,rss_mb,connect_ms,duration_s,mode" > "$RESULTS"

# 軸固定
RELIABLE=r
SIZE=100
CONNS=10
MODE=echo
RATE=50
LOSS=0
DURATION=20

PORT_BASE=30000
PORT=$PORT_BASE

for lib in ${LIBS//,/ }; do
  PORT=$((PORT + 1))

  S_OUT="$TMP/s_${lib}.csv"
  C_OUT="$TMP/c_${lib}.csv"

  if [ "$lib" = "litenetlib" ]; then
    if [ ! -x "$LITENETLIB_BIN" ]; then
      echo "litenetlib binary not found: $LITENETLIB_BIN — skipping" >&2
      continue
    fi
    WARMUP_ARG="$LITENETLIB_WARMUP"
    TIMEOUT_S=$((DURATION + LITENETLIB_WARMUP + 10))
    timeout "${TIMEOUT_S}s" "$LITENETLIB_BIN" --library="$lib" --role=server --port="$PORT" \
      --reliable="$RELIABLE" --duration="$DURATION" --warmup="$WARMUP_ARG" --loss="$LOSS" \
      --mode="$MODE" --out="$S_OUT" &
    SPID=$!
    sleep 0.5
    timeout "${TIMEOUT_S}s" "$LITENETLIB_BIN" --library="$lib" --role=client \
      --host=127.0.0.1 --port="$PORT" \
      --reliable="$RELIABLE" --size="$SIZE" --conns="$CONNS" --rate="$RATE" \
      --duration="$DURATION" --warmup="$WARMUP_ARG" --loss="$LOSS" --mode="$MODE" \
      --out="$C_OUT" || true
  else
    timeout 60s "$BIN" --library="$lib" --role=server --port="$PORT" \
      --reliable="$RELIABLE" --duration="$DURATION" --warmup=2 --loss="$LOSS" \
      --mode="$MODE" --out="$S_OUT" &
    SPID=$!
    sleep 0.2
    timeout 60s "$BIN" --library="$lib" --role=client \
      --host=127.0.0.1 --port="$PORT" \
      --reliable="$RELIABLE" --size="$SIZE" --conns="$CONNS" --rate="$RATE" \
      --duration="$DURATION" --warmup=2 --loss="$LOSS" --mode="$MODE" \
      --out="$C_OUT" || true
  fi

  kill "$SPID" 2>/dev/null || true
  wait "$SPID" 2>/dev/null || true

  if [ -f "$C_OUT" ]; then
    tail -n +2 "$C_OUT" >> "$RESULTS"
  fi
done

echo "wrote $RESULTS"
wc -l "$RESULTS"
