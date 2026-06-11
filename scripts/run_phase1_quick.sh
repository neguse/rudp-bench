#!/usr/bin/env bash
# Phase 1 quick smoke sweep — axis を 1 シナリオに固定して 11 lib 一巡(~5min)。
# 軸: size=100, conns=10, mode=echo, rate-r=50 rate-u=0 (reliable のみ既定),
#     loss=0, duration=20, warmup=2 (litenetlib のみ 5)
#
# --rate-r=<hz>, --rate-u=<hz> で混合トラフィックも指定可能(両方>0でHoL検証用)。
# 少なくとも一方は >0 必須。
#
# Usage:
#   scripts/run_phase1_quick.sh [--libraries=raw_udp,...] [--rate-r=50] [--rate-u=0] [--mode=echo|broadcast]

set -euo pipefail

LIBS="raw_udp,mini_rudp,coop_rudp,apex_rudp,enet,kcp,slikenet,raknet,udt4,yojimbo,gns,litenetlib,msquic"
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
LOSS=0
TAIL_MS=500
CLIENT_PROCS=1
DURATION=20
MODE=echo
# Per-lib ramp: msquic alone needs spread-out connects (its workers race when
# all 200 conns handshake at once).
# NOTE (2026-06-12): the earlier observation that "other libs degrade when
# ramp_up_ms > 0" (enet dr 0.62 at ramp=10s) was a harness artifact, not a
# library property: the server lifetime did not account for ramp_up_ms, so it
# exited mid-run and the tail of the client's send window was lost
# (dr ≈ (duration - ramp + 2s) / duration). Fixed in harness/runner.cc by
# extending the server deadline by ramp_up_ms. The same artifact is what made
# msquic's canonical capacity collapse to conns=1 across all profiles.
RAMP_UP_MS_DEFAULT=0
RAMP_UP_MS_MSQUIC=10000

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
    --loss=*) LOSS="${arg#*=}" ;;
    --tail-ms=*) TAIL_MS="${arg#*=}" ;;
    --duration=*) DURATION="${arg#*=}" ;;
    --mode=*) MODE="${arg#*=}" ;;
    --client-procs=*) CLIENT_PROCS="${arg#*=}" ;;
    --ramp-up-ms=*) RAMP_UP_MS_DEFAULT="${arg#*=}"; RAMP_UP_MS_MSQUIC="${arg#*=}" ;;
    *) echo "unknown arg: $arg" >&2; exit 2 ;;
  esac
done

if [ "$RATE_R" -le 0 ] && [ "$RATE_U" -le 0 ]; then
  echo "at least one of --rate-r / --rate-u must be > 0" >&2
  exit 2
fi
if [ "$CLIENT_PROCS" -lt 1 ]; then
  echo "--client-procs must be >= 1" >&2
  exit 2
fi
if [ "$CLIENT_PROCS" -gt "$CONNS" ]; then
  echo "--client-procs ($CLIENT_PROCS) must be <= --conns ($CONNS)" >&2
  exit 2
fi

if [ "$IDLE" != "spin" ] && [ "$IDLE" != "adaptive" ]; then
  echo "invalid --idle: $IDLE" >&2
  exit 2
fi
if [ "$MODE" != "echo" ] && [ "$MODE" != "broadcast" ]; then
  echo "invalid --mode: $MODE (echo|broadcast)" >&2
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
    SYSTEMD_ENV_ARGS=()
    if [ "${ENET_NO_THROTTLE+x}" = x ]; then
      SYSTEMD_ENV_ARGS+=("-E" "ENET_NO_THROTTLE=$ENET_NO_THROTTLE")
    fi
    if [ "${ENET_BATCH_POLL+x}" = x ]; then
      SYSTEMD_ENV_ARGS+=("-E" "ENET_BATCH_POLL=$ENET_BATCH_POLL")
    fi
    if [ "${ENET_POOL+x}" = x ]; then
      SYSTEMD_ENV_ARGS+=("-E" "ENET_POOL=$ENET_POOL")
    fi
    if [ "${ENET_RCVBUF_KB+x}" = x ]; then
      SYSTEMD_ENV_ARGS+=("-E" "ENET_RCVBUF_KB=$ENET_RCVBUF_KB")
    fi
    if [ "${ENET_PEERCOUNT+x}" = x ]; then
      SYSTEMD_ENV_ARGS+=("-E" "ENET_PEERCOUNT=$ENET_PEERCOUNT")
    fi
    if [ "${ENET_INITIAL_RTT_MS+x}" = x ]; then
      SYSTEMD_ENV_ARGS+=("-E" "ENET_INITIAL_RTT_MS=$ENET_INITIAL_RTT_MS")
    fi
    if [ "${ENET_INITIAL_RTT_VAR_MS+x}" = x ]; then
      SYSTEMD_ENV_ARGS+=("-E" "ENET_INITIAL_RTT_VAR_MS=$ENET_INITIAL_RTT_VAR_MS")
    fi
    if [ "${ENET_PING_MS+x}" = x ]; then
      SYSTEMD_ENV_ARGS+=("-E" "ENET_PING_MS=$ENET_PING_MS")
    fi
    if [ "${ENET_TIMEOUT_LIMIT+x}" = x ]; then
      SYSTEMD_ENV_ARGS+=("-E" "ENET_TIMEOUT_LIMIT=$ENET_TIMEOUT_LIMIT")
    fi
    if [ "${ENET_TIMEOUT_MIN_MS+x}" = x ]; then
      SYSTEMD_ENV_ARGS+=("-E" "ENET_TIMEOUT_MIN_MS=$ENET_TIMEOUT_MIN_MS")
    fi
    if [ "${ENET_TIMEOUT_MAX_MS+x}" = x ]; then
      SYSTEMD_ENV_ARGS+=("-E" "ENET_TIMEOUT_MAX_MS=$ENET_TIMEOUT_MAX_MS")
    fi
    if [ "${ENET_UNRELIABLE_MODE+x}" = x ]; then
      SYSTEMD_ENV_ARGS+=("-E" "ENET_UNRELIABLE_MODE=$ENET_UNRELIABLE_MODE")
    fi
    if [ "${ENET_UNSEQUENCED+x}" = x ]; then
      SYSTEMD_ENV_ARGS+=("-E" "ENET_UNSEQUENCED=$ENET_UNSEQUENCED")
    fi
    if [ "${RUDP_SERVER_RECV_DRAIN_LIMIT+x}" = x ]; then
      SYSTEMD_ENV_ARGS+=("-E" "RUDP_SERVER_RECV_DRAIN_LIMIT=$RUDP_SERVER_RECV_DRAIN_LIMIT")
    fi
    if [ "${APEX_RCVBUF_KB+x}" = x ]; then
      SYSTEMD_ENV_ARGS+=("-E" "APEX_RCVBUF_KB=$APEX_RCVBUF_KB")
    fi
    if [ "${APEX_ASYNC_SEND+x}" = x ]; then
      SYSTEMD_ENV_ARGS+=("-E" "APEX_ASYNC_SEND=$APEX_ASYNC_SEND")
    fi
    if [ "${APEX_ASYNC_UNRELIABLE_SERVER+x}" = x ]; then
      SYSTEMD_ENV_ARGS+=("-E" "APEX_ASYNC_UNRELIABLE_SERVER=$APEX_ASYNC_UNRELIABLE_SERVER")
    fi
    if [ "${APEX_ACK_DELAY_MS+x}" = x ]; then
      SYSTEMD_ENV_ARGS+=("-E" "APEX_ACK_DELAY_MS=$APEX_ACK_DELAY_MS")
    fi
    if [ "${APEX_RX_WORKER+x}" = x ]; then
      SYSTEMD_ENV_ARGS+=("-E" "APEX_RX_WORKER=$APEX_RX_WORKER")
    fi
    if [ "${APEX_SPLIT_ACK+x}" = x ]; then
      SYSTEMD_ENV_ARGS+=("-E" "APEX_SPLIT_ACK=$APEX_SPLIT_ACK")
    fi
    if [ "${APEX_RECV_DRAIN_ON_EMPTY+x}" = x ]; then
      SYSTEMD_ENV_ARGS+=("-E" "APEX_RECV_DRAIN_ON_EMPTY=$APEX_RECV_DRAIN_ON_EMPTY")
    fi
    if [ "${APEX_RECV_EMPTY_DRAINS+x}" = x ]; then
      SYSTEMD_ENV_ARGS+=("-E" "APEX_RECV_EMPTY_DRAINS=$APEX_RECV_EMPTY_DRAINS")
    fi
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
      -p LimitCORE=infinity \
      -p LimitNOFILE=524288 \
      -p RuntimeMaxSec="${timeout_s}s" \
      "${SYSTEMD_ENV_ARGS[@]}" \
      --quiet --wait --pipe --collect \
      "$@"
  elif [ -n "$cpu" ]; then
    timeout "${timeout_s}s" taskset -c "$cpu" "$@"
  else
    timeout "${timeout_s}s" "$@"
  fi
}

PORT_BASE=30000
PORT=$PORT_BASE

for lib in ${LIBS//,/ }; do
  PORT=$((PORT + 1))

  if [ "$lib" = "msquic" ]; then
    RAMP_UP_MS="$RAMP_UP_MS_MSQUIC"
  else
    RAMP_UP_MS="$RAMP_UP_MS_DEFAULT"
  fi

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
    TAIL_TIMEOUT_S=$(((TAIL_MS + 999) / 1000))
    RAMP_TIMEOUT_S=$(((RAMP_UP_MS + 999) / 1000))
    TIMEOUT_S=$((DURATION + LITENETLIB_WARMUP + RAMP_TIMEOUT_S + TAIL_TIMEOUT_S + 10))
    if [ ! -x "$LITENETLIB_BIN" ]; then
      : >"$S_STDOUT"
      : >"$C_STDOUT"
      echo "litenetlib binary not found: $LITENETLIB_BIN" >"$S_STDERR"
      echo "litenetlib binary not found: $LITENETLIB_BIN" >"$C_STDERR"
      S_STATUS=127
      C_STATUS=127
    else
      run_timeout "$SERVER_CPU" "$TIMEOUT_S" server "$LITENETLIB_BIN" --library="$lib" --role=server --port="$PORT" \
        --rate-r="$RATE_R" --rate-u="$RATE_U" --duration="$DURATION" --warmup="$WARMUP_ARG" --ramp-up-ms="$RAMP_UP_MS" --tail-ms="$TAIL_MS" --loss="$LOSS" \
        --size="$SIZE" --conns="$CONNS" --mode="$MODE" --idle="$IDLE" --out="$S_OUT" \
        >"$S_STDOUT" 2>"$S_STDERR" &
      SPID=$!
      sleep 0.5
      set +e
      if [ "$CLIENT_PROCS" -eq 1 ]; then
        run_timeout "$CLIENT_CPU" "$TIMEOUT_S" client "$LITENETLIB_BIN" --library="$lib" --role=client \
          --host=127.0.0.1 --port="$PORT" \
          --rate-r="$RATE_R" --rate-u="$RATE_U" --size="$SIZE" --conns="$CONNS" \
          --duration="$DURATION" --warmup="$WARMUP_ARG" --ramp-up-ms="$RAMP_UP_MS" --tail-ms="$TAIL_MS" --loss="$LOSS" --mode="$MODE" --idle="$IDLE" \
          --out="$C_OUT" >"$C_STDOUT" 2>"$C_STDERR"
        C_STATUS=$?
      else
        # Multi-process litenetlib client farm: N procs share CONNS, each emits
        # RTT bin sidecars (LatencyHist binary); combine_clients.py merges them
        # into $C_OUT just like the C++ multi-proc path.
        CPIDS=()
        COMBINE_ARGS=()
        CONN_OFFSET=0
        for i in $(seq 0 $((CLIENT_PROCS - 1))); do
          CONNS_I=$((CONNS / CLIENT_PROCS))
          if [ "$i" -lt $((CONNS % CLIENT_PROCS)) ]; then
            CONNS_I=$((CONNS_I + 1))
          fi
          C_OUT_I="$RAW_DIR/c_${SCENARIO_ID}_p${i}.csv"
          BINS_R_I="$RAW_DIR/c_${SCENARIO_ID}_p${i}_r.bin"
          BINS_U_I="$RAW_DIR/c_${SCENARIO_ID}_p${i}_u.bin"
          C_STDOUT_I="$RAW_DIR/c_${SCENARIO_ID}_p${i}.stdout.log"
          C_STDERR_I="$RAW_DIR/c_${SCENARIO_ID}_p${i}.stderr.log"
          run_timeout "$CLIENT_CPU" "$TIMEOUT_S" client "$LITENETLIB_BIN" --library="$lib" --role=client \
            --host=127.0.0.1 --port="$PORT" \
            --rate-r="$RATE_R" --rate-u="$RATE_U" --size="$SIZE" --conns="$CONNS_I" \
            --fanout-conns="$CONNS" --conn-id-offset="$CONN_OFFSET" \
            --duration="$DURATION" --warmup="$WARMUP_ARG" --ramp-up-ms="$RAMP_UP_MS" --tail-ms="$TAIL_MS" --loss="$LOSS" --mode="$MODE" --idle="$IDLE" \
            --out="$C_OUT_I" --bins-r-out="$BINS_R_I" --bins-u-out="$BINS_U_I" \
            >"$C_STDOUT_I" 2>"$C_STDERR_I" &
          CPIDS+=("$!")
          COMBINE_ARGS+=(--client-csv="$C_OUT_I" --bins-r="$BINS_R_I" --bins-u="$BINS_U_I")
          CONN_OFFSET=$((CONN_OFFSET + CONNS_I))
        done
        C_STATUS=0
        for pid in "${CPIDS[@]}"; do
          wait "$pid"
          STATUS=$?
          if [ "$STATUS" -ne 0 ] && [ "$C_STATUS" -eq 0 ]; then
            C_STATUS="$STATUS"
          fi
        done
        if [ "$C_STATUS" -eq 0 ]; then
          python3 scripts/combine_clients.py "${COMBINE_ARGS[@]}" \
            --out="$C_OUT" --conns-total="$CONNS"
          C_STATUS=$?
        fi
        cat "$RAW_DIR"/c_"${SCENARIO_ID}"_p*.stdout.log > "$C_STDOUT" 2>/dev/null || true
        cat "$RAW_DIR"/c_"${SCENARIO_ID}"_p*.stderr.log > "$C_STDERR" 2>/dev/null || true
      fi
      wait "$SPID" 2>/dev/null
      S_STATUS=$?
      set -e
    fi
  else
    TAIL_TIMEOUT_S=$(((TAIL_MS + 999) / 1000))
    RAMP_TIMEOUT_S=$(((RAMP_UP_MS + 999) / 1000))
    TIMEOUT_S=$((DURATION + 2 + RAMP_TIMEOUT_S + TAIL_TIMEOUT_S + 10))
    run_timeout "$SERVER_CPU" "$TIMEOUT_S" server "$BIN" --library="$lib" --role=server --port="$PORT" \
      --rate-r="$RATE_R" --rate-u="$RATE_U" --duration="$DURATION" --warmup=2 --ramp-up-ms="$RAMP_UP_MS" --tail-ms="$TAIL_MS" --loss="$LOSS" \
      --size="$SIZE" --conns="$CONNS" --mode="$MODE" --idle="$IDLE" --out="$S_OUT" \
      >"$S_STDOUT" 2>"$S_STDERR" &
    SPID=$!
    sleep 0.2
    set +e
    if [ "$CLIENT_PROCS" -eq 1 ]; then
      run_timeout "$CLIENT_CPU" "$TIMEOUT_S" client "$BIN" --library="$lib" --role=client \
        --host=127.0.0.1 --port="$PORT" \
        --rate-r="$RATE_R" --rate-u="$RATE_U" --size="$SIZE" --conns="$CONNS" \
        --duration="$DURATION" --warmup=2 --ramp-up-ms="$RAMP_UP_MS" --tail-ms="$TAIL_MS" --loss="$LOSS" --mode="$MODE" --idle="$IDLE" \
        --out="$C_OUT" >"$C_STDOUT" 2>"$C_STDERR"
      C_STATUS=$?
    else
      # Multi-process: spawn N clients sharing CONNS, each with bin sidecars.
      # combine_clients.py then sums counts + merges bins into the single
      # canonical $C_OUT consumed by reduce_result.py.
      CPIDS=()
      COMBINE_ARGS=()
      CONN_OFFSET=0
      for i in $(seq 0 $((CLIENT_PROCS - 1))); do
        CONNS_I=$((CONNS / CLIENT_PROCS))
        if [ "$i" -lt $((CONNS % CLIENT_PROCS)) ]; then
          CONNS_I=$((CONNS_I + 1))
        fi
        C_OUT_I="$RAW_DIR/c_${SCENARIO_ID}_p${i}.csv"
        BINS_R_I="$RAW_DIR/c_${SCENARIO_ID}_p${i}_r.bin"
        BINS_U_I="$RAW_DIR/c_${SCENARIO_ID}_p${i}_u.bin"
        C_STDOUT_I="$RAW_DIR/c_${SCENARIO_ID}_p${i}.stdout.log"
        C_STDERR_I="$RAW_DIR/c_${SCENARIO_ID}_p${i}.stderr.log"
        run_timeout "$CLIENT_CPU" "$TIMEOUT_S" client "$BIN" --library="$lib" --role=client \
          --host=127.0.0.1 --port="$PORT" \
          --rate-r="$RATE_R" --rate-u="$RATE_U" --size="$SIZE" --conns="$CONNS_I" \
          --fanout-conns="$CONNS" --conn-id-offset="$CONN_OFFSET" \
          --duration="$DURATION" --warmup=2 --ramp-up-ms="$RAMP_UP_MS" --tail-ms="$TAIL_MS" --loss="$LOSS" --mode="$MODE" --idle="$IDLE" \
          --out="$C_OUT_I" --bins-r-out="$BINS_R_I" --bins-u-out="$BINS_U_I" \
          >"$C_STDOUT_I" 2>"$C_STDERR_I" &
        CPIDS+=("$!")
        COMBINE_ARGS+=(--client-csv="$C_OUT_I" --bins-r="$BINS_R_I" --bins-u="$BINS_U_I")
        CONN_OFFSET=$((CONN_OFFSET + CONNS_I))
      done
      C_STATUS=0
      for pid in "${CPIDS[@]}"; do
        wait "$pid"
        STATUS=$?
        if [ "$STATUS" -ne 0 ] && [ "$C_STATUS" -eq 0 ]; then
          C_STATUS="$STATUS"
        fi
      done
      if [ "$C_STATUS" -eq 0 ]; then
        python3 scripts/combine_clients.py "${COMBINE_ARGS[@]}" \
          --out="$C_OUT" --conns-total="$CONNS"
        C_STATUS=$?
      fi
      # Concatenate per-proc client stdout/stderr so the diagnostic
      # links still point to one log per role.
      cat "$RAW_DIR"/c_"${SCENARIO_ID}"_p*.stdout.log > "$C_STDOUT" 2>/dev/null || true
      cat "$RAW_DIR"/c_"${SCENARIO_ID}"_p*.stderr.log > "$C_STDERR" 2>/dev/null || true
    fi
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
    --duration "$DURATION" --warmup "$WARMUP_ARG" --ramp-up-ms "$RAMP_UP_MS" --tail-ms "$TAIL_MS" --idle "$IDLE" \
    --server-cpu-pin "$SERVER_CPU" --client-cpu-pin "$CLIENT_CPU"
done

echo "wrote $RESULTS"
wc -l "$RESULTS"
echo "wrote $DIAGNOSTICS"
wc -l "$DIAGNOSTICS"
echo "wrote $SCENARIOS"
wc -l "$SCENARIOS"
