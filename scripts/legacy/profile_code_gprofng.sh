#!/usr/bin/env bash
# Collect a code-level server profile for one benchmark scenario.
#
# The server runs under gprofng. The client runs normally so the profiler
# overhead is isolated to the side we are investigating.

set -euo pipefail

PROFILE="game_server"
LIBRARY="apex_rudp"
BUILD_DIR="build-profile"
OUT=""
DURATION=20
WARMUP=2
TAIL_MS=500
HARNESS_LOSS=0
IDLE="adaptive"
PORT=30101
SERVER_CPU="7,15"
CLIENT_CPU="5,6,13,14"
DO_BUILD=1
DO_NETEM=1
NETEM_DELAY_MS=25
NETEM_JITTER_MS=5
NETEM_LOSS_PCT=1
NETEM_LIMIT=100000
GPROFNG_PERIOD="hi"

MODE_SET=0
RATE_R_SET=0
RATE_U_SET=0
SIZE_SET=0
CONNS_SET=0
MODE=""
RATE_R=""
RATE_U=""
SIZE=""
CONNS=""

usage() {
  cat <<EOF
usage: $0 [options]

Profiles server-side C++ hot paths with gprofng.

Options:
  --profile=game_server|media_relay|echo
  --library=apex_rudp
  --conns=N
  --mode=echo|broadcast
  --rate-r=HZ
  --rate-u=HZ
  --size=BYTES
  --duration=SECONDS
  --build-dir=DIR
  --out=DIR
  --port=N
  --server-cpu=LIST
  --client-cpu=LIST
  --no-build
  --no-netem
  --netem-delay-ms=N
  --netem-jitter-ms=N
  --netem-loss-pct=N
  --gprofng-period=lo|hi|on|MS

Default scenario is apex_rudp game_server at the current break point:
  broadcast, reliable 1Hz + unreliable 20Hz, 128B, 96 conns.
EOF
}

for arg in "$@"; do
  case "$arg" in
    --profile=*) PROFILE="${arg#*=}" ;;
    --library=*) LIBRARY="${arg#*=}" ;;
    --conns=*) CONNS="${arg#*=}"; CONNS_SET=1 ;;
    --mode=*) MODE="${arg#*=}"; MODE_SET=1 ;;
    --rate-r=*) RATE_R="${arg#*=}"; RATE_R_SET=1 ;;
    --rate-u=*) RATE_U="${arg#*=}"; RATE_U_SET=1 ;;
    --size=*) SIZE="${arg#*=}"; SIZE_SET=1 ;;
    --duration=*) DURATION="${arg#*=}" ;;
    --build-dir=*) BUILD_DIR="${arg#*=}" ;;
    --out=*) OUT="${arg#*=}" ;;
    --port=*) PORT="${arg#*=}" ;;
    --server-cpu=*) SERVER_CPU="${arg#*=}" ;;
    --client-cpu=*) CLIENT_CPU="${arg#*=}" ;;
    --no-build) DO_BUILD=0 ;;
    --no-netem) DO_NETEM=0 ;;
    --netem-delay-ms=*) NETEM_DELAY_MS="${arg#*=}" ;;
    --netem-jitter-ms=*) NETEM_JITTER_MS="${arg#*=}" ;;
    --netem-loss-pct=*) NETEM_LOSS_PCT="${arg#*=}" ;;
    --netem-limit=*) NETEM_LIMIT="${arg#*=}" ;;
    --gprofng-period=*) GPROFNG_PERIOD="${arg#*=}" ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown arg: $arg" >&2; usage >&2; exit 2 ;;
  esac
done

case "$PROFILE" in
  media_relay)
    DEFAULT_MODE="broadcast"
    DEFAULT_RATE_R=0
    DEFAULT_RATE_U=30
    DEFAULT_SIZE=1000
    DEFAULT_CONNS=75
    ;;
  game_server)
    DEFAULT_MODE="broadcast"
    DEFAULT_RATE_R=1
    DEFAULT_RATE_U=20
    DEFAULT_SIZE=128
    DEFAULT_CONNS=96
    ;;
  echo)
    DEFAULT_MODE="echo"
    DEFAULT_RATE_R=50
    DEFAULT_RATE_U=50
    DEFAULT_SIZE=64
    DEFAULT_CONNS=1500
    ;;
  *)
    echo "invalid --profile: $PROFILE" >&2
    exit 2
    ;;
esac

[ "$MODE_SET" -eq 1 ] || MODE="$DEFAULT_MODE"
[ "$RATE_R_SET" -eq 1 ] || RATE_R="$DEFAULT_RATE_R"
[ "$RATE_U_SET" -eq 1 ] || RATE_U="$DEFAULT_RATE_U"
[ "$SIZE_SET" -eq 1 ] || SIZE="$DEFAULT_SIZE"
[ "$CONNS_SET" -eq 1 ] || CONNS="$DEFAULT_CONNS"

if [ "$RATE_R" -le 0 ] && [ "$RATE_U" -le 0 ]; then
  echo "at least one of --rate-r / --rate-u must be > 0" >&2
  exit 2
fi
if [ "$MODE" != "echo" ] && [ "$MODE" != "broadcast" ]; then
  echo "invalid --mode: $MODE" >&2
  exit 2
fi
if ! command -v gprofng >/dev/null; then
  echo "gprofng not found" >&2
  exit 2
fi
if [ -n "$SERVER_CPU" ] || [ -n "$CLIENT_CPU" ]; then
  command -v taskset >/dev/null || { echo "taskset not found" >&2; exit 2; }
fi

if [ -z "$OUT" ]; then
  TS="$(date -u +%Y%m%dT%H%M%SZ)"
  OUT="results/code_profile_${PROFILE}_${LIBRARY}_c${CONNS}_${TS}"
fi

BIN="$BUILD_DIR/harness/rudp-bench"
SCENARIO_ID="${PROFILE}_${LIBRARY}_r${RATE_R}_u${RATE_U}_${SIZE}_${CONNS}_${MODE}_${HARNESS_LOSS}_${IDLE}"
SERVER_CSV="$OUT/server.csv"
CLIENT_CSV="$OUT/client.csv"
RESULTS_CSV="$OUT/results.csv"
DIAGNOSTICS_CSV="$OUT/diagnostics.csv"
SCENARIOS_CSV="$OUT/scenarios.csv"
SERVER_EXP="$OUT/server.er"

mkdir -p "$OUT"

if [ "$DO_BUILD" -eq 1 ]; then
  cmake -S . -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_CXX_FLAGS_RELWITHDEBINFO="-O2 -g -fno-omit-frame-pointer -DNDEBUG" \
    -DCMAKE_C_FLAGS_RELWITHDEBINFO="-O2 -g -fno-omit-frame-pointer -DNDEBUG" \
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
  cmake --build "$BUILD_DIR" --target rudp-bench -j
fi

[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 2; }

cleanup() {
  if [ -n "${SERVER_PID:-}" ] && kill -0 "$SERVER_PID" 2>/dev/null; then
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
  fi
  if [ "$DO_NETEM" -eq 1 ]; then
    sudo scripts/netem.sh clear >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT INT TERM

{
  echo "profile=$PROFILE"
  echo "library=$LIBRARY"
  echo "scenario_id=$SCENARIO_ID"
  echo "mode=$MODE"
  echo "rate_r=$RATE_R"
  echo "rate_u=$RATE_U"
  echo "size=$SIZE"
  echo "conns=$CONNS"
  echo "duration=$DURATION"
  echo "warmup=$WARMUP"
  echo "tail_ms=$TAIL_MS"
  echo "harness_loss=$HARNESS_LOSS"
  echo "idle=$IDLE"
  echo "netem=$DO_NETEM"
  echo "netem_delay_ms=$NETEM_DELAY_MS"
  echo "netem_jitter_ms=$NETEM_JITTER_MS"
  echo "netem_loss_pct=$NETEM_LOSS_PCT"
  echo "server_cpu=$SERVER_CPU"
  echo "client_cpu=$CLIENT_CPU"
  echo "gprofng_period=$GPROFNG_PERIOD"
  echo
  git rev-parse --short HEAD 2>/dev/null || true
  git status --short 2>/dev/null || true
  echo
  env | sort | grep -E '^(APEX_|RUDP_|ENET_|GNS_|KCP_|MINI_|RAW_|MSQUIC_|UDT4_|YOJIMBO_|SLIKENET_)' || true
} >"$OUT/run_context.txt"

if [ "$DO_NETEM" -eq 1 ]; then
  sudo scripts/netem.sh apply "$NETEM_DELAY_MS" "$NETEM_JITTER_MS" "$NETEM_LOSS_PCT" "$NETEM_LIMIT" \
    >"$OUT/netem_apply.log" 2>&1
fi

python3 scripts/reduce_result.py init \
  --results "$RESULTS_CSV" --diagnostics "$DIAGNOSTICS_CSV" --scenarios "$SCENARIOS_CSV"

SERVER_CMD=("$BIN" --library="$LIBRARY" --role=server --port="$PORT"
  --rate-r="$RATE_R" --rate-u="$RATE_U" --duration="$DURATION" --warmup="$WARMUP"
  --ramp-up-ms=0 --tail-ms="$TAIL_MS" --loss="$HARNESS_LOSS"
  --size="$SIZE" --conns="$CONNS" --mode="$MODE" --idle="$IDLE" --out="$SERVER_CSV")
CLIENT_CMD=("$BIN" --library="$LIBRARY" --role=client --host=127.0.0.1 --port="$PORT"
  --rate-r="$RATE_R" --rate-u="$RATE_U" --duration="$DURATION" --warmup="$WARMUP"
  --ramp-up-ms=0 --tail-ms="$TAIL_MS" --loss="$HARNESS_LOSS"
  --size="$SIZE" --conns="$CONNS" --mode="$MODE" --idle="$IDLE" --out="$CLIENT_CSV")

printf '%q ' "${SERVER_CMD[@]}" >"$OUT/server.command.txt"
printf '\n' >>"$OUT/server.command.txt"
printf '%q ' "${CLIENT_CMD[@]}" >"$OUT/client.command.txt"
printf '\n' >>"$OUT/client.command.txt"

SERVER_PREFIX=()
CLIENT_PREFIX=()
[ -z "$SERVER_CPU" ] || SERVER_PREFIX=(taskset -c "$SERVER_CPU")
[ -z "$CLIENT_CPU" ] || CLIENT_PREFIX=(taskset -c "$CLIENT_CPU")

TAIL_TIMEOUT_S=$(((TAIL_MS + 999) / 1000))
TIMEOUT_S=$((DURATION + WARMUP + TAIL_TIMEOUT_S + 15))

"${SERVER_PREFIX[@]}" timeout "${TIMEOUT_S}s" \
  gprofng collect app -p "$GPROFNG_PERIOD" -S on -O "$SERVER_EXP" \
  "${SERVER_CMD[@]}" >"$OUT/server.stdout.log" 2>"$OUT/server.stderr.log" &
SERVER_PID=$!

sleep 0.5

set +e
"${CLIENT_PREFIX[@]}" timeout "${TIMEOUT_S}s" \
  "${CLIENT_CMD[@]}" >"$OUT/client.stdout.log" 2>"$OUT/client.stderr.log"
CLIENT_STATUS=$?
wait "$SERVER_PID"
SERVER_STATUS=$?
SERVER_PID=""
set -e

python3 scripts/reduce_result.py append \
  --results "$RESULTS_CSV" --diagnostics "$DIAGNOSTICS_CSV" --scenarios "$SCENARIOS_CSV" \
  --server "$SERVER_CSV" --client "$CLIENT_CSV" \
  --server-stdout "$OUT/server.stdout.log" --server-stderr "$OUT/server.stderr.log" \
  --client-stdout "$OUT/client.stdout.log" --client-stderr "$OUT/client.stderr.log" \
  --server-status "$SERVER_STATUS" --client-status "$CLIENT_STATUS" \
  --run-id "$SCENARIO_ID" --scenario-id "$SCENARIO_ID" \
  --library "$LIBRARY" --rate-r "$RATE_R" --rate-u "$RATE_U" \
  --size "$SIZE" --conns "$CONNS" --loss "$HARNESS_LOSS" --mode "$MODE" \
  --duration "$DURATION" --warmup "$WARMUP" --ramp-up-ms=0 --tail-ms "$TAIL_MS" --idle "$IDLE" \
  --server-cpu-pin "$SERVER_CPU" --client-cpu-pin "$CLIENT_CPU"

cat >"$OUT/gprofng.script" <<EOF
outfile $OUT/profile_overview.txt
overview
outfile $OUT/profile_functions.txt
limit 80
metrics e.%totalcpu:i.%totalcpu:name
sort e.%totalcpu
functions
outfile $OUT/profile_calltree.txt
limit 160
calltree
outfile $OUT/profile_threads.txt
thread_list
outfile $OUT/profile_lines.txt
limit 80
lines
outfile $OUT/profile_ioactivity.txt
limit 80
ioactivity
exit
EOF

gprofng display text -script "$OUT/gprofng.script" "$SERVER_EXP" \
  >"$OUT/gprofng_display.stdout.log" 2>"$OUT/gprofng_display.stderr.log" || true

{
  echo "output=$OUT"
  echo "server_status=$SERVER_STATUS"
  echo "client_status=$CLIENT_STATUS"
  echo
  echo "result:"
  sed -n '1,3p' "$RESULTS_CSV"
  echo
  echo "top functions:"
  sed -n '1,45p' "$OUT/profile_functions.txt" 2>/dev/null || true
} | tee "$OUT/summary.txt"
