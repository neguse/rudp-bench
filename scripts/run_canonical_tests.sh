#!/usr/bin/env bash
# Canonical benchmark test entrypoint.
#
# "Test" here means the full canonical benchmark sweep: build the harness, then
# run the current final saturation profiles for every canonical target.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"

CMAKE="${CMAKE:-cmake}"
PYTHON="${PYTHON:-python3}"
BUILD_DIR="${BUILD_DIR:-build}"
OUT="${OUT:-results/canonical_final_benchmark_$(date -u +%Y%m%dT%H%M%SZ)}"
UPDATE_SUBMODULES="${UPDATE_SUBMODULES:-1}"
DRY_RUN="${DRY_RUN:-0}"
PUBLISH_DOCS="${PUBLISH_DOCS:-1}"
PUBLISH_ID="${PUBLISH_ID:-$(date -u +%Y-%m-%d-canonical-%H%M%SZ)}"

CANONICAL_LIBS="raw_udp,mini_rudp,coop_rudp,apex_rudp,enet,kcp,slikenet,raknet,udt4,yojimbo,gns,litenetlib,msquic"
CANONICAL_RUNS="1 2 3"
CANONICAL_NETEM_ARGS="25 5 1 100000"
CANONICAL_MEDIA_CONNS="1 5 50 75 100 125 150 200"
CANONICAL_GAME_CONNS="1 5 64 96 128 192 256"
CANONICAL_ECHO_CONNS="1 50 200 600 1000 1500 2000 3000"
CANONICAL_RELIABLE_ECHO_CONNS="1 50 200 600 1000 1500 2000 3000"

usage() {
  cat <<USAGE
Usage: scripts/run_canonical_tests.sh [options]

Runs the canonical full benchmark sweep, not unit tests.

Options:
  --out PATH               Output directory (default: $OUT)
  --build-dir PATH         CMake build directory (default: $BUILD_DIR)
  --no-submodule-update    Skip git submodule update
  --no-publish-docs        Do not publish docs/measurements/<id> or update current.md
  --dry-run                Print commands without executing them
  -h, --help               Show this help

Environment:
  JOBS=N                   Parallel build jobs
  CMAKE=/path/to/cmake     CMake executable
  PYTHON=/path/to/python3  Python executable
USAGE
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --out)
      OUT="$2"
      shift 2
      ;;
    --out=*)
      OUT="${1#*=}"
      shift
      ;;
    --build-dir)
      BUILD_DIR="$2"
      shift 2
      ;;
    --build-dir=*)
      BUILD_DIR="${1#*=}"
      shift
      ;;
    --no-submodule-update)
      UPDATE_SUBMODULES=0
      shift
      ;;
    --no-publish-docs)
      PUBLISH_DOCS=0
      shift
      ;;
    --dry-run)
      DRY_RUN=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "unknown arg: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ "$BUILD_DIR" != /* ]]; then
  BUILD_DIR="$ROOT/$BUILD_DIR"
fi
if [[ "$OUT" != /* ]]; then
  OUT="$ROOT/$OUT"
fi

if [ -z "${JOBS:-}" ]; then
  if command -v nproc >/dev/null 2>&1; then
    JOBS="$(nproc)"
  elif getconf _NPROCESSORS_ONLN >/dev/null 2>&1; then
    JOBS="$(getconf _NPROCESSORS_ONLN)"
  else
    JOBS=2
  fi
fi

run_cmd() {
  printf '+'
  printf ' %q' "$@"
  printf '\n'
  if [ "$DRY_RUN" != "1" ]; then
    "$@"
  fi
}

command -v "$CMAKE" >/dev/null 2>&1 || { echo "cmake not found: $CMAKE" >&2; exit 2; }
command -v "$PYTHON" >/dev/null 2>&1 || { echo "python not found: $PYTHON" >&2; exit 2; }

cd "$ROOT"

echo "==> canonical benchmark test"
echo "==> repo: $ROOT"
echo "==> build dir: $BUILD_DIR"
echo "==> output dir: $OUT"
echo "==> libraries: $CANONICAL_LIBS"
echo "==> runs: $CANONICAL_RUNS"

if [ "$UPDATE_SUBMODULES" != "0" ]; then
  run_cmd git -C "$ROOT" submodule update --init --recursive
fi

run_cmd "$CMAKE" -S "$ROOT" -B "$BUILD_DIR"
run_cmd "$CMAKE" --build "$BUILD_DIR" -j "$JOBS"

BENCH_CMD=(
  "$PYTHON" scripts/run_final_saturation_profiles.py
  --out "$OUT"
  --build-dir "$BUILD_DIR"
  --libraries "$CANONICAL_LIBS"
  --runs "$CANONICAL_RUNS"
  --netem 1
  --netem-args "$CANONICAL_NETEM_ARGS"
  --duration 20
  --tail-ms 500
  --idle adaptive
  --isolate systemd
  --server-cpu 7,15
  --client-cpu 3,4,5,6,11,12,13,14
  --min-valid 2
  --min-delivery 0.95
  --media-conns "$CANONICAL_MEDIA_CONNS"
  --game-conns "$CANONICAL_GAME_CONNS"
  --echo-conns "$CANONICAL_ECHO_CONNS"
  --reliable-echo-conns "$CANONICAL_RELIABLE_ECHO_CONNS"
  --broadcast-client-procs 8
  --echo-client-procs 8
)

run_cmd "${BENCH_CMD[@]}"

REPORT_CMD=(
  "$PYTHON" scripts/render_canonical_report.py
  --run-dir "$OUT"
)
run_cmd "${REPORT_CMD[@]}"

if [ "$PUBLISH_DOCS" != "0" ]; then
  PUBLISH_CMD=(
    "$PYTHON" scripts/publish_canonical_result.py
    --run-dir "$OUT"
    --dest "$ROOT/docs/measurements/$PUBLISH_ID"
    --current "$ROOT/docs/measurements/current.md"
  )
  run_cmd "${PUBLISH_CMD[@]}"
fi

if [ "$DRY_RUN" != "1" ]; then
  if command -v tc >/dev/null 2>&1; then
    tc qdisc show dev lo | tee "$OUT/qdisc_after.txt"
  fi
  echo "==> canonical benchmark complete"
  echo "==> report: $OUT/report.md"
  if [ "$PUBLISH_DOCS" != "0" ]; then
    echo "==> published docs report: $ROOT/docs/measurements/$PUBLISH_ID/report.md"
    echo "==> current docs pointer: $ROOT/docs/measurements/current.md"
  fi
  echo "==> capacity: $OUT/capacity.csv"
  echo "==> summary: $OUT/summary.csv"
fi
