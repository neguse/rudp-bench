#!/usr/bin/env bash
# Phase 1 quick smoke sweep — axis を 1 シナリオに固定して 10 lib 一巡(~5min)。
# 軸: size=100, conns=10, mode=echo, rate-r=50 rate-u=0 (reliable のみ既定),
#     loss=0, duration=20, warmup=2 (litenetlib のみ 5)
#
# --rate-r=<hz>, --rate-u=<hz> で混合トラフィックも指定可能(両方>0でHoL検証用)。
# 少なくとも一方は >0 必須。
#
# Usage:
#   scripts/run_phase1_quick.sh [--libraries=raw_udp,...] [--rate-r=50] [--rate-u=0]

set -euo pipefail

LIBS="raw_udp,mini_rudp,enet,kcp,slikenet,udt4,yojimbo,gns,litenetlib,msquic"
BUILD_DIR="build"
RESULTS="results/phase1_quick.csv"
DIAGNOSTICS=""
SCENARIOS=""
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)"
RAW_DIR=""
IDLE="spin"
SERVER_CPU=""
CLIENT_CPU=""
ISOLATE="taskset"
LITENETLIB_BIN="adapters/litenetlib/bin/Release/net10.0/litenetlib_adapter"
SIZE=100
CONNS=10
RATE_R=50
RATE_U=0

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
    --server-cpu=*) SERVER_CPU="${arg#*=}" ;;
    --client-cpu=*) CLIENT_CPU="${arg#*=}" ;;
    --isolate=*) ISOLATE="${arg#*=}" ;;
    --litenetlib-bin=*) LITENETLIB_BIN="${arg#*=}" ;;
    --conns=*) CONNS="${arg#*=}" ;;
    --size=*) SIZE="${arg#*=}" ;;
    --rate-r=*) RATE_R="${arg#*=}" ;;
    --rate-u=*) RATE_U="${arg#*=}" ;;
    *) echo "unknown arg: $arg" >&2; exit 2 ;;
  esac
done

if [ "$RATE_R" -le 0 ] && [ "$RATE_U" -le 0 ]; then
  echo "at least one of --rate-r / --rate-u must be > 0" >&2
  exit 2
fi

if [ "$IDLE" != "spin" ] && [ "$IDLE" != "adaptive" ]; then
  echo "invalid --idle: $IDLE" >&2
  exit 2
fi
if [ "$ISOLATE" != "taskset" ] && [ "$ISOLATE" != "systemd" ]; then
  echo "invalid --isolate: $ISOLATE (taskset|systemd)" >&2
  exit 2
fi
if { [ -n "$SERVER_CPU" ] || [ -n "$CLIENT_CPU" ]; } && [ "$ISOLATE" = "taskset" ] && ! command -v taskset >/dev/null; then
  echo "--server-cpu/--client-cpu with --isolate=taskset require taskset" >&2
  exit 2
fi
if [ "$ISOLATE" = "systemd" ] && ! command -v systemd-run >/dev/null; then
  echo "--isolate=systemd requires systemd-run" >&2
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

LITENETLIB_WARMUP=5

mkdir -p "$(dirname "$RESULTS")" "$(dirname "$DIAGNOSTICS")" "$(dirname "$SCENARIOS")" "$RAW_DIR"

python3 scripts/reduce_result.py init \
  --results "$RESULTS" --diagnostics "$DIAGNOSTICS" --scenarios "$SCENARIOS"

run_timeout() {
  local cpu="$1"
  local timeout_s="$2"
  local label="$3"  # server|client, used as slice suffix in systemd mode
  shift 3
  if [ "$ISOLATE" = "systemd" ] && [ -n "$cpu" ]; then
    # Unit is killed by systemd after RuntimeMaxSec; also wrap with timeout
    # as a belt-and-suspenders against systemd-run hanging.
    # WorkingDirectory preserves CWD so relative --out paths resolve correctly.
    # User=$USER drops privileges from sudo's root back to the invoking user
    # so libraries that refuse to run as root (msquic) survive.
    timeout "$((timeout_s + 5))s" sudo systemd-run \
      --slice="bench-${label}.slice" \
      --working-directory="$PWD" \
      -p AllowedCPUs="$cpu" -p CPUWeight=10000 \
      -p User="$USER" \
      -p RuntimeMaxSec="${timeout_s}s" \
      --quiet --wait --pipe --collect \
      "$@"
  elif [ -n "$cpu" ]; then
    timeout "${timeout_s}s" taskset -c "$cpu" "$@"
  else
    timeout "${timeout_s}s" "$@"
  fi
}

# 軸固定 (size, conns, rate-r, rate-u は CLI で上書き可能)
MODE=echo
LOSS=0
DURATION=20

PORT_BASE=30000
PORT=$PORT_BASE

for lib in ${LIBS//,/ }; do
  PORT=$((PORT + 1))

  SCENARIO_ID="${lib}_r${RATE_R}_u${RATE_U}_${SIZE}_${CONNS}_${MODE}_${LOSS}_${IDLE}"
  S_OUT="$RAW_DIR/s_${SCENARIO_ID}.csv"
  C_OUT="$RAW_DIR/c_${SCENARIO_ID}.csv"
  S_STDOUT="$RAW_DIR/s_${SCENARIO_ID}.stdout.log"
  S_STDERR="$RAW_DIR/s_${SCENARIO_ID}.stderr.log"
  C_STDOUT="$RAW_DIR/c_${SCENARIO_ID}.stdout.log"
  C_STDERR="$RAW_DIR/c_${SCENARIO_ID}.stderr.log"
  WARMUP_ARG=2
  S_STATUS=0
  C_STATUS=0

  if [ "$lib" = "litenetlib" ]; then
    WARMUP_ARG="$LITENETLIB_WARMUP"
    TIMEOUT_S=$((DURATION + LITENETLIB_WARMUP + 10))
    if [ ! -x "$LITENETLIB_BIN" ]; then
      : >"$S_STDOUT"
      : >"$C_STDOUT"
      echo "litenetlib binary not found: $LITENETLIB_BIN" >"$S_STDERR"
      echo "litenetlib binary not found: $LITENETLIB_BIN" >"$C_STDERR"
      S_STATUS=127
      C_STATUS=127
    else
      run_timeout "$SERVER_CPU" "$TIMEOUT_S" server "$LITENETLIB_BIN" --library="$lib" --role=server --port="$PORT" \
        --rate-r="$RATE_R" --rate-u="$RATE_U" --duration="$DURATION" --warmup="$WARMUP_ARG" --loss="$LOSS" \
        --size="$SIZE" --conns="$CONNS" --mode="$MODE" --idle="$IDLE" --out="$S_OUT" \
        >"$S_STDOUT" 2>"$S_STDERR" &
      SPID=$!
      sleep 0.5
      set +e
      run_timeout "$CLIENT_CPU" "$TIMEOUT_S" client "$LITENETLIB_BIN" --library="$lib" --role=client \
        --host=127.0.0.1 --port="$PORT" \
        --rate-r="$RATE_R" --rate-u="$RATE_U" --size="$SIZE" --conns="$CONNS" \
        --duration="$DURATION" --warmup="$WARMUP_ARG" --loss="$LOSS" --mode="$MODE" --idle="$IDLE" \
        --out="$C_OUT" >"$C_STDOUT" 2>"$C_STDERR"
      C_STATUS=$?
      wait "$SPID" 2>/dev/null
      S_STATUS=$?
      set -e
    fi
  else
    run_timeout "$SERVER_CPU" 60 server "$BIN" --library="$lib" --role=server --port="$PORT" \
      --rate-r="$RATE_R" --rate-u="$RATE_U" --duration="$DURATION" --warmup=2 --loss="$LOSS" \
      --size="$SIZE" --conns="$CONNS" --mode="$MODE" --idle="$IDLE" --out="$S_OUT" \
      >"$S_STDOUT" 2>"$S_STDERR" &
    SPID=$!
    sleep 0.2
    set +e
    run_timeout "$CLIENT_CPU" 60 client "$BIN" --library="$lib" --role=client \
      --host=127.0.0.1 --port="$PORT" \
      --rate-r="$RATE_R" --rate-u="$RATE_U" --size="$SIZE" --conns="$CONNS" \
      --duration="$DURATION" --warmup=2 --loss="$LOSS" --mode="$MODE" --idle="$IDLE" \
      --out="$C_OUT" >"$C_STDOUT" 2>"$C_STDERR"
    C_STATUS=$?
    wait "$SPID" 2>/dev/null
    S_STATUS=$?
    set -e
  fi

  python3 scripts/reduce_result.py append \
    --results "$RESULTS" --diagnostics "$DIAGNOSTICS" --scenarios "$SCENARIOS" \
    --server "$S_OUT" --client "$C_OUT" \
    --server-stdout "$S_STDOUT" --server-stderr "$S_STDERR" \
    --client-stdout "$C_STDOUT" --client-stderr "$C_STDERR" \
    --server-status "$S_STATUS" --client-status "$C_STATUS" \
    --run-id "$RUN_ID" --scenario-id "$SCENARIO_ID" \
    --library "$lib" --rate-r "$RATE_R" --rate-u "$RATE_U" \
    --size "$SIZE" --conns "$CONNS" --loss "$LOSS" --mode "$MODE" \
    --duration "$DURATION" --warmup "$WARMUP_ARG" --idle "$IDLE" \
    --server-cpu-pin "$SERVER_CPU" --client-cpu-pin "$CLIENT_CPU"
done

echo "wrote $RESULTS"
wc -l "$RESULTS"
echo "wrote $DIAGNOSTICS"
wc -l "$DIAGNOSTICS"
echo "wrote $SCENARIOS"
wc -l "$SCENARIOS"
