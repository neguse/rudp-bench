#!/usr/bin/env bash
# Phase 1 sweep runner.
# Usage:
#   scripts/run_phase1.sh --libraries=raw_udp,mini_rudp [--build-dir=build] [--results=results/phase1.csv]
#
# 注意: --loss > 0 の組合せは sudo で tc qdisc を操作するため、--loss-injection を与えたとき
# のみ実際に netem を適用する。デフォルトはメタデータのみ書き込み(注入なし)。

set -euo pipefail

LIBS="raw_udp,mini_rudp,enet"
BUILD_DIR="build"
RESULTS="results/phase1.csv"
LOSS_INJECT="0"   # 0=skip, 1=apply via sudo

for arg in "$@"; do
  case "$arg" in
    --libraries=*) LIBS="${arg#*=}" ;;
    --build-dir=*) BUILD_DIR="${arg#*=}" ;;
    --results=*) RESULTS="${arg#*=}" ;;
    --loss-injection) LOSS_INJECT=1 ;;
    *) echo "unknown arg: $arg" >&2; exit 2 ;;
  esac
done

BIN="$BUILD_DIR/harness/rudp-bench"
[ -x "$BIN" ] || { echo "binary not found: $BIN" >&2; exit 2; }

mkdir -p "$(dirname "$RESULTS")"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"; if [ "$LOSS_INJECT" = "1" ]; then sudo scripts/set_loss.sh clear >/dev/null 2>&1 || true; fi' EXIT

# CSV header (1回だけ)
echo "library,encryption,phase,reliable,size,conns,rate,loss,throughput_mbps,msg_per_sec,rtt_p50_us,rtt_p95_us,rtt_p99_us,delivered,sent,delivery_ratio,cpu_pct,rss_mb,connect_ms,duration_s" > "$RESULTS"

PORT_BASE=30000
PORT=$PORT_BASE

for lib in ${LIBS//,/ }; do
  for reliable in r u; do
    for size in 64 65536; do
      for conns in 1 1000; do
        for rate in 100 100000; do   # 低 / 飽和超え近似(後でライブラリ別調整)
          for loss in 0 5; do
            PORT=$((PORT + 1))
            if [ "$LOSS_INJECT" = "1" ]; then
              sudo scripts/set_loss.sh apply "$loss" >/dev/null
            fi

            S_OUT="$TMP/s_${lib}_${reliable}_${size}_${conns}_${rate}_${loss}.csv"
            C_OUT="$TMP/c_${lib}_${reliable}_${size}_${conns}_${rate}_${loss}.csv"

            timeout 90s "$BIN" --library="$lib" --role=server --port="$PORT" \
              --reliable="$reliable" --duration=30 --warmup=2 --loss="$loss" \
              --out="$S_OUT" &
            SPID=$!
            sleep 0.2

            timeout 90s "$BIN" --library="$lib" --role=client \
              --host=127.0.0.1 --port="$PORT" \
              --reliable="$reliable" --size="$size" --conns="$conns" --rate="$rate" \
              --duration=30 --warmup=2 --loss="$loss" \
              --out="$C_OUT" || true

            kill "$SPID" 2>/dev/null || true
            wait "$SPID" 2>/dev/null || true

            # client 行のみ抽出して追記(server 行は CPU/RSS 別計測なので別ファイル)
            if [ -f "$C_OUT" ]; then
              tail -n +2 "$C_OUT" >> "$RESULTS"
            fi

            if [ "$LOSS_INJECT" = "1" ]; then
              sudo scripts/set_loss.sh clear >/dev/null
            fi
          done
        done
      done
    done
  done
done

echo "wrote $RESULTS"
wc -l "$RESULTS"
