#!/usr/bin/env bash
# Compatibility wrapper. The canonical benchmark entrypoint is the Go CLI:
#   go run ./cmd/rudp-bench-canonical

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"

cd "$ROOT"
exec go run ./cmd/rudp-bench-canonical "$@"
