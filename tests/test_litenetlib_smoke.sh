#!/usr/bin/env bash
# Smoke test for LiteNetLib (.NET) adapter.
# Starts server and client on loopback, verifies CSV output has delivery_ratio > 0.
# NOT a C++ GTest — registered in tests/CMakeLists.txt as a shell test.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ADAPTER="$REPO_ROOT/adapters/litenetlib/bin/Release/net10.0/litenetlib_adapter"

if [ ! -x "$ADAPTER" ]; then
    echo "litenetlib_adapter binary not found or not executable: $ADAPTER" >&2
    echo "Run: dotnet build -c Release in adapters/litenetlib/" >&2
    exit 1
fi

PORT=30209
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Start server in background
"$ADAPTER" --library=litenetlib --role=server --port="$PORT" \
    --rate-r=100 --rate-u=0 --duration=6 --warmup=2 --loss=0 \
    --out="$TMP/server.csv" &
SPID=$!

sleep 0.5

# Run client
"$ADAPTER" --library=litenetlib --role=client \
    --host=127.0.0.1 --port="$PORT" \
    --rate-r=100 --rate-u=0 --size=64 --conns=1 --duration=6 --warmup=2 --loss=0 \
    --out="$TMP/client.csv" || {
    kill "$SPID" 2>/dev/null || true
    echo "FAIL: client exited with error" >&2
    exit 1
}

kill "$SPID" 2>/dev/null || true
wait "$SPID" 2>/dev/null || true

if [ ! -f "$TMP/client.csv" ]; then
    echo "FAIL: client.csv not found" >&2
    exit 1
fi

echo "=== client.csv ==="
cat "$TMP/client.csv"

# delivery_ratio は CSV 16 列目 (1-indexed)
# header: library,encryption,phase,rate_r,rate_u,size,conns,loss,throughput_mbps,
#         msg_per_sec,rtt_p50_us,rtt_p95_us,rtt_p99_us,delivered,accepted,delivery_ratio,...
DELIVERY_RATIO=$(tail -n1 "$TMP/client.csv" | cut -d',' -f16)
echo "delivery_ratio = $DELIVERY_RATIO"

if awk "BEGIN { exit ($DELIVERY_RATIO > 0) ? 0 : 1 }"; then
    echo "PASS: delivery_ratio > 0"
else
    echo "FAIL: delivery_ratio = $DELIVERY_RATIO, expected > 0" >&2
    exit 1
fi
