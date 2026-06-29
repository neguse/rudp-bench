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

csv_value() {
    local column="$1"
    local file="$2"
    awk -F, -v target="$column" '
    NR == 1 {
        for (i = 1; i <= NF; i++) if ($i == target) col = i
        next
    }
    NR == 2 && col { print $col }
    ' "$file"
}

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

DELIVERY_RATIO=$(csv_value delivery_ratio "$TMP/client.csv")
echo "delivery_ratio = $DELIVERY_RATIO"

if awk "BEGIN { exit ($DELIVERY_RATIO > 0) ? 0 : 1 }"; then
    echo "PASS: delivery_ratio > 0"
else
    echo "FAIL: delivery_ratio = $DELIVERY_RATIO, expected > 0" >&2
    exit 1
fi

CAP_PORT=30210
LNL_OUTGOING_BYTES=1 "$ADAPTER" --library=litenetlib --role=server --port="$CAP_PORT" \
    --rate-r=20 --rate-u=0 --duration=1 --warmup=0 --loss=0 \
    --out="$TMP/cap_server.csv" &
SPID=$!

sleep 0.5

LNL_OUTGOING_BYTES=1 "$ADAPTER" --library=litenetlib --role=client \
    --host=127.0.0.1 --port="$CAP_PORT" \
    --rate-r=20 --rate-u=0 --size=64 --conns=1 --duration=1 --warmup=0 --loss=0 \
    --out="$TMP/cap_client.csv" || {
    kill "$SPID" 2>/dev/null || true
    echo "FAIL: capped client exited with error" >&2
    exit 1
}

kill "$SPID" 2>/dev/null || true
wait "$SPID" 2>/dev/null || true

echo "=== cap_client.csv ==="
cat "$TMP/cap_client.csv"

CAP_ATTEMPTED=$(csv_value client_attempted "$TMP/cap_client.csv")
CAP_ACCEPTED=$(csv_value client_accepted "$TMP/cap_client.csv")
echo "capped attempted = $CAP_ATTEMPTED, accepted = $CAP_ACCEPTED"

if awk "BEGIN { exit ($CAP_ATTEMPTED > 0 && $CAP_ACCEPTED == 0) ? 0 : 1 }"; then
    echo "PASS: LNL_OUTGOING_BYTES applies backpressure"
else
    echo "FAIL: expected capped accepted=0 with attempted>0" >&2
    exit 1
fi
