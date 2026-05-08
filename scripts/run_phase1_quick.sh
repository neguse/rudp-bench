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
DIAGNOSTICS=""
SCENARIOS=""
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)"
RAW_DIR=""
IDLE="spin"
SIZE=100
CONNS=10

for arg in "$@"; do
  case "$arg" in
    --libraries=*) LIBS="${arg#*=}" ;;
    --build-dir=*) BUILD_DIR="${arg#*=}" ;;
    --results=*) RESULTS="${arg#*=}" ;;
    --diagnostics=*) DIAGNOSTICS="${arg#*=}" ;;
    --scenarios=*) SCENARIOS="${arg#*=}" ;;
    --run-id=*) RUN_ID="${arg#*=}" ;;
    --raw-dir=*) RAW_DIR="${arg#*=}" ;;
    --idle=*) IDLE="${arg#*=}" ;;
    --conns=*) CONNS="${arg#*=}" ;;
    --size=*) SIZE="${arg#*=}" ;;
    *) echo "unknown arg: $arg" >&2; exit 2 ;;
  esac
done

if [ "$IDLE" != "spin" ] && [ "$IDLE" != "adaptive" ]; then
  echo "invalid --idle: $IDLE" >&2
  exit 2
fi

if [ -z "$DIAGNOSTICS" ]; then
  DIAGNOSTICS="${RESULTS%.csv}_diagnostics.csv"
fi
if [ -z "$SCENARIOS" ]; then
  SCENARIOS="${RESULTS%.csv}_scenarios.csv"
fi
if [ -z "$RAW_DIR" ]; then
  RAW_DIR="${RESULTS%.csv}_raw/$RUN_ID"
fi

BIN="$BUILD_DIR/harness/rudp-bench"
[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 2; }

LITENETLIB_BIN="adapters/litenetlib/bin/Release/net10.0/litenetlib_adapter"
LITENETLIB_WARMUP=5

mkdir -p "$(dirname "$RESULTS")" "$(dirname "$DIAGNOSTICS")" "$(dirname "$SCENARIOS")" "$RAW_DIR"

python3 scripts/reduce_result.py init \
  --results "$RESULTS" --diagnostics "$DIAGNOSTICS" --scenarios "$SCENARIOS"

# 軸固定 (size, conns は --size, --conns で上書き可能)
RELIABLE=r
MODE=echo
RATE=50
LOSS=0
DURATION=20

PORT_BASE=30000
PORT=$PORT_BASE

for lib in ${LIBS//,/ }; do
  PORT=$((PORT + 1))

  SCENARIO_ID="${lib}_${RELIABLE}_${SIZE}_${CONNS}_${RATE}_${MODE}_${LOSS}_${IDLE}"
  S_OUT="$RAW_DIR/s_${SCENARIO_ID}.csv"
  C_OUT="$RAW_DIR/c_${SCENARIO_ID}.csv"
  WARMUP_ARG=2
  S_STATUS=0
  C_STATUS=0

  if [ "$lib" = "litenetlib" ]; then
    if [ ! -x "$LITENETLIB_BIN" ]; then
      echo "litenetlib binary not found: $LITENETLIB_BIN — skipping" >&2
      continue
    fi
    WARMUP_ARG="$LITENETLIB_WARMUP"
    TIMEOUT_S=$((DURATION + LITENETLIB_WARMUP + 10))
    timeout "${TIMEOUT_S}s" "$LITENETLIB_BIN" --library="$lib" --role=server --port="$PORT" \
      --reliable="$RELIABLE" --duration="$DURATION" --warmup="$WARMUP_ARG" --loss="$LOSS" \
      --mode="$MODE" --idle="$IDLE" --out="$S_OUT" &
    SPID=$!
    sleep 0.5
    set +e
    timeout "${TIMEOUT_S}s" "$LITENETLIB_BIN" --library="$lib" --role=client \
      --host=127.0.0.1 --port="$PORT" \
      --reliable="$RELIABLE" --size="$SIZE" --conns="$CONNS" --rate="$RATE" \
      --duration="$DURATION" --warmup="$WARMUP_ARG" --loss="$LOSS" --mode="$MODE" --idle="$IDLE" \
      --out="$C_OUT"
    C_STATUS=$?
    wait "$SPID" 2>/dev/null
    S_STATUS=$?
    set -e
  else
    timeout 60s "$BIN" --library="$lib" --role=server --port="$PORT" \
      --reliable="$RELIABLE" --duration="$DURATION" --warmup=2 --loss="$LOSS" \
      --mode="$MODE" --idle="$IDLE" --out="$S_OUT" &
    SPID=$!
    sleep 0.2
    set +e
    timeout 60s "$BIN" --library="$lib" --role=client \
      --host=127.0.0.1 --port="$PORT" \
      --reliable="$RELIABLE" --size="$SIZE" --conns="$CONNS" --rate="$RATE" \
      --duration="$DURATION" --warmup=2 --loss="$LOSS" --mode="$MODE" --idle="$IDLE" \
      --out="$C_OUT"
    C_STATUS=$?
    wait "$SPID" 2>/dev/null
    S_STATUS=$?
    set -e
  fi

  python3 scripts/reduce_result.py append \
    --results "$RESULTS" --diagnostics "$DIAGNOSTICS" --scenarios "$SCENARIOS" \
    --server "$S_OUT" --client "$C_OUT" \
    --server-status "$S_STATUS" --client-status "$C_STATUS" \
    --run-id "$RUN_ID" --scenario-id "$SCENARIO_ID" \
    --library "$lib" --reliable "$RELIABLE" --size "$SIZE" --conns "$CONNS" \
    --rate "$RATE" --loss "$LOSS" --mode "$MODE" \
    --duration "$DURATION" --warmup "$WARMUP_ARG" --idle "$IDLE"
done

echo "wrote $RESULTS"
wc -l "$RESULTS"
echo "wrote $DIAGNOSTICS"
wc -l "$DIAGNOSTICS"
echo "wrote $SCENARIOS"
wc -l "$SCENARIOS"
