#!/usr/bin/env bash
# Phase 1 sweep runner.
# Usage:
#   scripts/run_phase1.sh --libraries=raw_udp,mini_rudp [--build-dir=build] [--results=results/phase1.csv]
#
# 注意: --loss > 0 の組合せは sudo で tc qdisc を操作するため、--loss-injection を与えたとき
# のみ実際に netem を適用する。デフォルトはメタデータのみ書き込み(注入なし)。

set -euo pipefail

LIBS="raw_udp,mini_rudp,enet,kcp,slikenet,udt4,yojimbo,gns,litenetlib,msquic"
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

# LiteNetLib は独立した .NET バイナリ。同じ CLI 仕様・CSV フォーマットを持つ。
LITENETLIB_BIN="adapters/litenetlib/bin/Release/net10.0/litenetlib_adapter"
# spec: JIT/GC ウォームアップのため warmup を 5 秒に引き上げる
LITENETLIB_WARMUP=5

mkdir -p "$(dirname "$RESULTS")"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"; if [ "$LOSS_INJECT" = "1" ]; then sudo scripts/set_loss.sh clear >/dev/null 2>&1 || true; fi' EXIT

# CSV header (1回だけ)
echo "library,encryption,phase,reliable,size,conns,rate,loss,throughput_mbps,msg_per_sec,rtt_p50_us,rtt_p95_us,rtt_p99_us,delivered,sent,delivery_ratio,cpu_pct,rss_mb,connect_ms,duration_s,mode" > "$RESULTS"

PORT_BASE=30000
PORT=$PORT_BASE

for lib in ${LIBS//,/ }; do
  for reliable in r u; do
    for size in 64 1000; do
      for conns in 1 50; do
        for mode in echo broadcast; do
          for loss in 0; do
            PORT=$((PORT + 1))
            rate=50  # 全シナリオ共通: 50 msg/sec/conn
            if [ "$LOSS_INJECT" = "1" ]; then
              sudo scripts/set_loss.sh apply "$loss" >/dev/null
            fi

            S_OUT="$TMP/s_${lib}_${reliable}_${size}_${conns}_${rate}_${mode}_${loss}.csv"
            C_OUT="$TMP/c_${lib}_${reliable}_${size}_${conns}_${rate}_${mode}_${loss}.csv"

            # LiteNetLib は独立 .NET バイナリに dispatch。warmup は 5 秒固定。
            if [ "$lib" = "litenetlib" ]; then
              if [ ! -x "$LITENETLIB_BIN" ]; then
                echo "litenetlib binary not found: $LITENETLIB_BIN — skipping" >&2
                continue
              fi
              WARMUP_ARG="$LITENETLIB_WARMUP"
              TIMEOUT_S=$((20 + LITENETLIB_WARMUP + 10))
              timeout "${TIMEOUT_S}s" "$LITENETLIB_BIN" --library="$lib" --role=server --port="$PORT" \
                --reliable="$reliable" --duration=20 --warmup="$WARMUP_ARG" --loss="$loss" \
                --mode="$mode" \
                --out="$S_OUT" &
              SPID=$!
              sleep 0.5
              timeout "${TIMEOUT_S}s" "$LITENETLIB_BIN" --library="$lib" --role=client \
                --host=127.0.0.1 --port="$PORT" \
                --reliable="$reliable" --size="$size" --conns="$conns" --rate="$rate" \
                --duration=20 --warmup="$WARMUP_ARG" --loss="$loss" --mode="$mode" \
                --out="$C_OUT" || true
            else
              timeout 60s "$BIN" --library="$lib" --role=server --port="$PORT" \
                --reliable="$reliable" --duration=20 --warmup=2 --loss="$loss" \
                --mode="$mode" \
                --out="$S_OUT" &
              SPID=$!
              sleep 0.2
              timeout 60s "$BIN" --library="$lib" --role=client \
                --host=127.0.0.1 --port="$PORT" \
                --reliable="$reliable" --size="$size" --conns="$conns" --rate="$rate" \
                --duration=20 --warmup=2 --loss="$loss" --mode="$mode" \
                --out="$C_OUT" || true
            fi

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
