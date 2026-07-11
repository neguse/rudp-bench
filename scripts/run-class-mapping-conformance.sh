#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd -P)"
SCRIPT="$ROOT/scripts/run-class-mapping-conformance.sh"
cd "$ROOT"

ORCHESTRATOR="${ORCHESTRATOR:-build-v2/orchestrator}"
CONFIG="${CONFIG:-orchestrator/examples/class-mapping-conformance-local.json}"
RIG="${RIG:-orchestrator/rigs/home.json}"
SESSION="${SESSION:-$(date -u +%Y%m%dT%H%M%SZ)-$$}"
SESSION_DIR="${SESSION_DIR:-results-v2/class-mapping-conformance/$SESSION}"
DOCTOR_REPORT="$SESSION_DIR/doctor.json"

if [[ "$EUID" -ne 0 ]]; then
  export ORCHESTRATOR CONFIG RIG SESSION SESSION_DIR
  exec sudo --preserve-env=ORCHESTRATOR,CONFIG,RIG,SESSION,SESSION_DIR \
    /bin/bash "$SCRIPT"
fi

if [[ ! "${SUDO_UID:-}" =~ ^[1-9][0-9]*$ || ! "${SUDO_GID:-}" =~ ^[1-9][0-9]*$ ]]; then
  printf '%s\n' \
    'run-class-mapping-conformance: invoke through sudo so SUT processes can drop privileges' >&2
  exit 2
fi
INVOKER_USER="$(id -nu "$SUDO_UID" 2>/dev/null || true)"
INVOKER_GID="$(id -g "$SUDO_UID" 2>/dev/null || true)"
INVOKER_HOME="$(getent passwd "$SUDO_UID" 2>/dev/null | cut -d: -f6 || true)"
if [[ -z "$INVOKER_USER" || "$INVOKER_GID" != "$SUDO_GID" || \
      -z "$INVOKER_HOME" || ! -d "$INVOKER_HOME" ]]; then
  printf '%s\n' 'run-class-mapping-conformance: invalid SUDO_UID/SUDO_GID identity' >&2
  exit 2
fi
if [[ -n "${SUDO_USER:-}" && "$SUDO_USER" != "$INVOKER_USER" ]]; then
  printf '%s\n' 'run-class-mapping-conformance: SUDO_USER does not match SUDO_UID' >&2
  exit 2
fi

BENCH_CPUS="$(jq -er '.bench_cpus | select(type == "string" and length > 0)' "$RIG")"
NOFILE_LIMIT="$(jq -er '.min_nofile | select(type == "number" and . >= 1024 and floor == .)' "$RIG")"

in_bench_slice() {
  local _ controllers cgroup_path
  while IFS=: read -r _ controllers cgroup_path; do
    if [[ "$cgroup_path" == /bench.slice || "$cgroup_path" == /bench.slice/* ||
          "$cgroup_path" == */bench.slice || "$cgroup_path" == */bench.slice/* ]]; then
      return 0
    fi
  done </proc/self/cgroup
  return 1
}

# The isolated rig moves system.slice/user.slice away from the benchmark CPUs.
# Re-enter once under bench.slice so the fixed taskset masks are valid.
if ! in_bench_slice; then
  if [[ "${RUDP_BENCH_CLASS_MAPPING_SCOPE:-0}" == 1 ]]; then
    printf '%s\n' 'run-class-mapping-conformance: systemd-run did not enter bench.slice' >&2
    exit 2
  fi
  UNIT="rudp-class-mapping-$(date +%s)-$$"
  exec systemd-run --wait --pipe --collect \
    --unit="$UNIT" \
    --slice=bench.slice \
    -p AllowedCPUs="$BENCH_CPUS" \
    -p CPUWeight=10000 \
    -p LimitNOFILE="$NOFILE_LIMIT" \
    -p WorkingDirectory="$ROOT" \
    /usr/bin/env \
    RUDP_BENCH_CLASS_MAPPING_SCOPE=1 \
    ORCHESTRATOR="$ORCHESTRATOR" \
    CONFIG="$CONFIG" \
    RIG="$RIG" \
    SESSION="$SESSION" \
    SESSION_DIR="$SESSION_DIR" \
    BENCH_CPUS="$BENCH_CPUS" \
    SUDO_UID="$SUDO_UID" \
    SUDO_GID="$SUDO_GID" \
    SUDO_USER="$INVOKER_USER" \
    HOME="$INVOKER_HOME" \
    USER="$INVOKER_USER" \
    LOGNAME="$INVOKER_USER" \
    /bin/bash "$SCRIPT"
fi

assert_no_symlink_components() {
  local path="$1"
  local label="$2"
  local absolute component current lexical resolved
  local -a components

  if [[ "$path" == *$'\n'* || "$path" == *$'\r'* ]]; then
    printf '%s contains an invalid newline: %s\n' "$label" "$path" >&2
    return 1
  fi
  if [[ "$path" == /* ]]; then
    absolute="$path"
  else
    absolute="$ROOT/$path"
  fi

  IFS='/' read -r -a components <<<"$absolute"
  current=/
  for component in "${components[@]}"; do
    case "$component" in
      ''|.)
        ;;
      ..)
        if [[ "$current" != / ]]; then
          current="${current%/*}"
          [[ -n "$current" ]] || current=/
        fi
        ;;
      *)
        current="${current%/}/$component"
        if [[ -L "$current" ]]; then
          printf '%s must not traverse a symlink: %s\n' "$label" "$path" >&2
          return 1
        fi
        ;;
    esac
  done

  lexical="$(realpath -ms -- "$path")"
  resolved="$(realpath -m -- "$path")"
  if [[ "$lexical" != "$resolved" ]]; then
    printf '%s must not traverse a symlink: %s\n' "$label" "$path" >&2
    return 1
  fi
}

assert_no_symlink_components "$SESSION_DIR" 'session directory'
setpriv --reuid "$SUDO_UID" --regid "$SUDO_GID" --init-groups \
  mkdir -p -- "$SESSION_DIR"
assert_no_symlink_components "$SESSION_DIR" 'session directory'
if [[ ! -d "$SESSION_DIR" || -L "$SESSION_DIR" ]]; then
  printf 'session path is not a regular directory: %s\n' "$SESSION_DIR" >&2
  exit 1
fi

assert_no_symlink_components "$DOCTOR_REPORT" 'doctor report'
if [[ ! -e "$DOCTOR_REPORT" ]]; then
  doctor_rc=0
  setpriv --reuid "$SUDO_UID" --regid "$SUDO_GID" --init-groups \
    "$ORCHESTRATOR" doctor -rig "$RIG" -repo . -output "$DOCTOR_REPORT" || doctor_rc=$?
  if [[ "$doctor_rc" -ne 0 && "$doctor_rc" -ne 2 ]]; then
    exit "$doctor_rc"
  fi
  assert_no_symlink_components "$DOCTOR_REPORT" 'doctor report'
fi
if [[ ! -f "$DOCTOR_REPORT" || -L "$DOCTOR_REPORT" ]]; then
  printf 'doctor report is not a regular file: %s\n' "$DOCTOR_REPORT" >&2
  exit 1
fi

command=(
  "$ORCHESTRATOR" mapping-conformance
  -config "$CONFIG"
  -output-dir "$SESSION_DIR"
  -doctor-report "$DOCTOR_REPORT"
)
"${command[@]}"

printf 'class-mapping conformance session: %s\n' "$SESSION_DIR"
