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

DELIVERY_RATIO=$(awk -F, '
NR == 1 {
    for (i = 1; i <= NF; i++) if ($i == "delivery_ratio") col = i
    next
}
NR == 2 && col { print $col }
' "$TMP/client.csv")
echo "delivery_ratio = $DELIVERY_RATIO"

if awk "BEGIN { exit ($DELIVERY_RATIO > 0) ? 0 : 1 }"; then
    echo "PASS: delivery_ratio > 0"
else
    echo "FAIL: delivery_ratio = $DELIVERY_RATIO, expected > 0" >&2
    exit 1
fi
