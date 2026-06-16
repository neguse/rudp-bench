#!/usr/bin/env bash
# Remove local generated benchmark/build artifacts. Versioned measurement
# reports under docs/measurements are intentionally not touched here.

set -euo pipefail

ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

usage() {
  cat <<USAGE
Usage: scripts/clean_generated.sh [--results] [--builds] [--all]

Options:
  --results  Remove ignored local benchmark outputs under results/
  --builds   Remove local CMake build directories
  --all      Remove both results and builds
  -h, --help Show this help
USAGE
}

CLEAN_RESULTS=0
CLEAN_BUILDS=0

if [ "$#" -eq 0 ]; then
  usage
  exit 2
fi

while [ "$#" -gt 0 ]; do
  case "$1" in
    --results)
      CLEAN_RESULTS=1
      ;;
    --builds)
      CLEAN_BUILDS=1
      ;;
    --all)
      CLEAN_RESULTS=1
      CLEAN_BUILDS=1
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
  shift
done

cd "$ROOT"

if [ "$CLEAN_RESULTS" = "1" ]; then
  mkdir -p results
  find results -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +
  echo "cleaned results/"
fi

if [ "$CLEAN_BUILDS" = "1" ]; then
  find . -maxdepth 1 -type d \( -name 'build' -o -name 'build-*' \) -exec rm -rf -- {} +
  echo "cleaned build directories"
fi
