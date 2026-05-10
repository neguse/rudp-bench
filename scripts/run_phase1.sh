#!/usr/bin/env bash
# Phase 1 sweep runner.
# Usage:
#   scripts/run_phase1.sh --libraries=raw_udp,mini_rudp [--build-dir=build] [--results=results/phase1.csv]
#   scripts/run_phase1.sh --sizes=64,1000 --conns=1,50 --rates=50 --losses=0 --modes=echo,broadcast --reliabilities=r,u
#
# 注意: --loss > 0 の組合せは sudo で tc qdisc を操作するため、--loss-injection を与えたとき
# のみ実際に netem を適用する。デフォルトはメタデータのみ書き込み(注入なし)。

set -euo pipefail

LIBS="raw_udp,mini_rudp,enet,kcp,slikenet,udt4,yojimbo,gns,litenetlib,msquic"
BUILD_DIR="build"
RESULTS="results/phase1.csv"
DIAGNOSTICS=""
SCENARIOS=""
RUN_ID="$(date -u +%Y%m%dT%H%M%SZ)"
RAW_DIR=""
IDLE="spin"
SERVER_CPU=""
CLIENT_CPU=""
RELIABILITIES="r,u"
SIZES="64,1000"
CONNS_SET="1,50"
RATES="50"
LOSSES="0"
MODES="echo,broadcast"
DURATION=20
WARMUP=2
LOSS_INJECT="0"   # 0=skip, 1=apply via sudo

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
    --reliabilities=*) RELIABILITIES="${arg#*=}" ;;
    --sizes=*) SIZES="${arg#*=}" ;;
    --conns=*) CONNS_SET="${arg#*=}" ;;
    --rates=*) RATES="${arg#*=}" ;;
    --losses=*) LOSSES="${arg#*=}" ;;
    --modes=*) MODES="${arg#*=}" ;;
    --duration=*) DURATION="${arg#*=}" ;;
    --warmup=*) WARMUP="${arg#*=}" ;;
    --loss-injection) LOSS_INJECT=1 ;;
    *) echo "unknown arg: $arg" >&2; exit 2 ;;
  esac
done

if [ "$IDLE" != "spin" ] && [ "$IDLE" != "adaptive" ]; then
  echo "invalid --idle: $IDLE" >&2
  exit 2
fi
if { [ -n "$SERVER_CPU" ] || [ -n "$CLIENT_CPU" ]; } && ! command -v taskset >/dev/null; then
  echo "--server-cpu/--client-cpu require taskset" >&2
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

# LiteNetLib は独立した .NET バイナリ。同じ CLI 仕様・CSV フォーマットを持つ。
LITENETLIB_BIN="adapters/litenetlib/bin/Release/net10.0/litenetlib_adapter"
# spec: JIT/GC ウォームアップのため default は 5 秒。--warmup 指定時は同じ値を使う。
LITENETLIB_WARMUP="$WARMUP"
if [ "$WARMUP" = "2" ]; then
  LITENETLIB_WARMUP=5
fi

mkdir -p "$(dirname "$RESULTS")" "$(dirname "$DIAGNOSTICS")" "$(dirname "$SCENARIOS")" "$RAW_DIR"
trap 'if [ "$LOSS_INJECT" = "1" ]; then sudo scripts/set_loss.sh clear >/dev/null 2>&1 || true; fi' EXIT

python3 scripts/reduce_result.py init \
  --results "$RESULTS" --diagnostics "$DIAGNOSTICS" --scenarios "$SCENARIOS"

run_timeout() {
  local cpu="$1"
  local timeout_s="$2"
  shift 2
  if [ -n "$cpu" ]; then
    timeout "${timeout_s}s" taskset -c "$cpu" "$@"
  else
    timeout "${timeout_s}s" "$@"
  fi
}

PORT_BASE=30000
PORT=$PORT_BASE

for lib in ${LIBS//,/ }; do
  for reliable in ${RELIABILITIES//,/ }; do
    for size in ${SIZES//,/ }; do
      for conns in ${CONNS_SET//,/ }; do
        for mode in ${MODES//,/ }; do
          for rate in ${RATES//,/ }; do
            for loss in ${LOSSES//,/ }; do
              PORT=$((PORT + 1))
              if [ "$LOSS_INJECT" = "1" ]; then
                sudo scripts/set_loss.sh apply "$loss" >/dev/null
              fi

              SCENARIO_ID="${lib}_${reliable}_${size}_${conns}_${rate}_${mode}_${loss}_${IDLE}"
              S_OUT="$RAW_DIR/s_${SCENARIO_ID}.csv"
              C_OUT="$RAW_DIR/c_${SCENARIO_ID}.csv"
              WARMUP_ARG="$WARMUP"
              S_STATUS=0
              C_STATUS=0

              # LiteNetLib は独立 .NET バイナリに dispatch。
              if [ "$lib" = "litenetlib" ]; then
                if [ ! -x "$LITENETLIB_BIN" ]; then
                  echo "litenetlib binary not found: $LITENETLIB_BIN — skipping" >&2
                  continue
                fi
                WARMUP_ARG="$LITENETLIB_WARMUP"
                TIMEOUT_S=$((DURATION + LITENETLIB_WARMUP + 10))
                run_timeout "$SERVER_CPU" "$TIMEOUT_S" "$LITENETLIB_BIN" --library="$lib" --role=server --port="$PORT" \
                  --reliable="$reliable" --duration="$DURATION" --warmup="$WARMUP_ARG" --loss="$loss" \
                  --size="$size" --conns="$conns" --rate="$rate" --mode="$mode" --idle="$IDLE" \
                  --out="$S_OUT" &
                SPID=$!
                sleep 0.5
                set +e
                run_timeout "$CLIENT_CPU" "$TIMEOUT_S" "$LITENETLIB_BIN" --library="$lib" --role=client \
                  --host=127.0.0.1 --port="$PORT" \
                  --reliable="$reliable" --size="$size" --conns="$conns" --rate="$rate" \
                  --duration="$DURATION" --warmup="$WARMUP_ARG" --loss="$loss" --mode="$mode" --idle="$IDLE" \
                  --out="$C_OUT"
                C_STATUS=$?
                wait "$SPID" 2>/dev/null
                S_STATUS=$?
                set -e
              else
                TIMEOUT_S=$((DURATION + WARMUP + 10))
                run_timeout "$SERVER_CPU" "$TIMEOUT_S" "$BIN" --library="$lib" --role=server --port="$PORT" \
                  --reliable="$reliable" --duration="$DURATION" --warmup="$WARMUP" --loss="$loss" \
                  --size="$size" --conns="$conns" --rate="$rate" --mode="$mode" --idle="$IDLE" \
                  --out="$S_OUT" &
                SPID=$!
                sleep 0.2
                set +e
                run_timeout "$CLIENT_CPU" "$TIMEOUT_S" "$BIN" --library="$lib" --role=client \
                  --host=127.0.0.1 --port="$PORT" \
                  --reliable="$reliable" --size="$size" --conns="$conns" --rate="$rate" \
                  --duration="$DURATION" --warmup="$WARMUP" --loss="$loss" --mode="$mode" --idle="$IDLE" \
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
                --library "$lib" --reliable "$reliable" --size "$size" --conns "$conns" \
                --rate "$rate" --loss "$loss" --mode "$mode" \
                --duration "$DURATION" --warmup "$WARMUP_ARG" --idle "$IDLE" \
                --server-cpu-pin "$SERVER_CPU" --client-cpu-pin "$CLIENT_CPU"

              if [ "$LOSS_INJECT" = "1" ]; then
                sudo scripts/set_loss.sh clear >/dev/null
              fi
            done
          done
        done
      done
    done
  done
done

echo "wrote $RESULTS"
wc -l "$RESULTS"
echo "wrote $DIAGNOSTICS"
wc -l "$DIAGNOSTICS"
echo "wrote $SCENARIOS"
wc -l "$SCENARIOS"
