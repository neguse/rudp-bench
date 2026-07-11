#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

ORCHESTRATOR="${ORCHESTRATOR:-build-v2/orchestrator}"
SESSION="${SESSION:-$(date -u +%Y%m%dT%H%M%SZ)-$$}"
SCRIPT="$ROOT/scripts/run-provisional-loss-smoke.sh"
RIG_CONFIG="$ROOT/orchestrator/rigs/home.json"

if [[ $EUID -ne 0 ]]; then
  printf '%s\n' 'run-provisional-loss-smoke: root is required (run with sudo)' >&2
  exit 2
fi
if [[ -z "${SUDO_UID:-}" || -z "${SUDO_GID:-}" ]]; then
  printf '%s\n' 'run-provisional-loss-smoke: invoke through sudo so SUT processes can drop privileges' >&2
  exit 2
fi
INVOKER_USER="$(id -nu "$SUDO_UID")"
INVOKER_HOME="$(getent passwd "$SUDO_UID" | cut -d: -f6)"
if [[ -z "$INVOKER_USER" || -z "$INVOKER_HOME" || ! -d "$INVOKER_HOME" ]]; then
  printf '%s\n' 'run-provisional-loss-smoke: cannot resolve invoking user home' >&2
  exit 2
fi

# The isolated rig moves system.slice/user.slice off the benchmark CPUs. Enter
# bench.slice once so the taskset masks in the configs remain usable.
if [[ "${RUDP_BENCH_PROVISIONAL_LOSS_SCOPE:-0}" != 1 ]]; then
  BENCH_CPUS="$(jq -er '.bench_cpus' "$RIG_CONFIG")"
  UNIT="rudp-provisional-loss-$(date +%s)-$$"
  exec systemd-run --wait --pipe --collect \
    --unit="$UNIT" \
    --slice=bench.slice \
    -p AllowedCPUs="$BENCH_CPUS" \
    -p CPUWeight=10000 \
    -p WorkingDirectory="$ROOT" \
    /usr/bin/env \
    RUDP_BENCH_PROVISIONAL_LOSS_SCOPE=1 \
    ORCHESTRATOR="$ORCHESTRATOR" \
    SESSION="$SESSION" \
    SUDO_UID="$SUDO_UID" \
    SUDO_GID="$SUDO_GID" \
    SUDO_USER="$INVOKER_USER" \
    HOME="$INVOKER_HOME" \
    USER="$INVOKER_USER" \
    LOGNAME="$INVOKER_USER" \
    /bin/bash "$SCRIPT"
fi

SESSION_DIR="results-v2/provisional-loss-smoke-sessions/$SESSION"
DOCTOR_REPORT="$SESSION_DIR/doctor.json"
BASELINE_CONFIG="orchestrator/examples/environment-baseline-loss-provisional.json"
SWEEP_TEMPLATE="orchestrator/examples/sweep-scenario-loss-provisional.json"
SWEEP_CONFIG="$SESSION_DIR/sweep-config.json"
BUNDLE_MANIFEST="$SESSION_DIR/bundle-manifest.json"

cleanup_netns() {
  ip netns del rudpbench-srv 2>/dev/null || true
  ip netns del rudpbench-cli 2>/dev/null || true
}

# Recover from a previously interrupted run and cover every normal/error exit.
cleanup_netns
trap cleanup_netns EXIT

mkdir -p "$SESSION_DIR"
chown "$SUDO_UID:$SUDO_GID" "$SESSION_DIR"

doctor_rc=0
setpriv --reuid "$SUDO_UID" --regid "$SUDO_GID" --init-groups \
  "$ORCHESTRATOR" doctor \
  -rig orchestrator/rigs/home.json \
  -output "$DOCTOR_REPORT" || doctor_rc=$?
if [[ "$doctor_rc" -ne 0 && "$doctor_rc" -ne 2 ]]; then
  exit "$doctor_rc"
fi

# This provisional pipeline smoke records a Doctor FAIL but does not promote
# the run to conformance, a frozen preset, or reference evidence.
JQ_DOCTOR_REPORT="$DOCTOR_REPORT" jq \
  '.doctor_report = env.JQ_DOCTOR_REPORT' \
  "$SWEEP_TEMPLATE" >"$SWEEP_CONFIG"

"$ORCHESTRATOR" run \
  -config "$BASELINE_CONFIG" \
  -output-dir "$SESSION_DIR/environment-baseline-before"

"$ORCHESTRATOR" sweep \
  -config "$SWEEP_CONFIG" \
  -output-dir "$SESSION_DIR/solutions"

"$ORCHESTRATOR" run \
  -config "$BASELINE_CONFIG" \
  -output-dir "$SESSION_DIR/environment-baseline-after"

jq -e '
  .outcome == "PASS" and .verdict == "VALID"
' "$SESSION_DIR/environment-baseline-before/result.json" >/dev/null

jq -e '
  .outcome == "PASS" and .verdict == "VALID"
' "$SESSION_DIR/environment-baseline-after/result.json" >/dev/null

expected_cells='[
  "enet|authoritative-state-provisional-loss",
  "enet|room-relay-provisional-loss",
  "websocket|authoritative-state-provisional-loss",
  "websocket|room-relay-provisional-loss"
]'

jq -e --argjson expected "$expected_cells" '
  (.cells | length) == 4 and
  ([.cells[] | "\(.transport)|\(.scenario)"] | sort) == ($expected | sort) and
  all(.cells[];
    .regime == "provisional-loss-pipeline-smoke-v1" and
    ((.outcome // "") == "") and
    ((.measurement_invalid // false) == false) and
    ((.censored // false) == false))
' "$SESSION_DIR/solutions/capacity.json" >/dev/null

jq -s -e --argjson expected "$expected_cells" '
  length >= 4 and
  all(.[];
    .conns == 3 and
    .regime == "provisional-loss-pipeline-smoke-v1" and
    .verdict == "VALID" and
    (.outcome == "PASS" or .outcome == "FAIL") and
    ((.acquisition_id | type) == "string") and
    ((.acquisition_id | length) == 64)) and
  ([.[] | "\(.transport)|\(.scenario)"] | unique | sort) == ($expected | sort)
' "$SESSION_DIR/solutions/results.jsonl" >/dev/null

python3 scripts/session_bundle_manifest.py create "$SESSION_DIR"

sha256_file() {
  sha256sum "$1" | awk '{print $1}'
}

jq -n \
  --slurpfile doctor "$DOCTOR_REPORT" \
  --slurpfile baseline_before "$SESSION_DIR/environment-baseline-before/result.json" \
  --slurpfile baseline_after "$SESSION_DIR/environment-baseline-after/result.json" \
  --slurpfile points "$SESSION_DIR/solutions/results.jsonl" \
  --arg generated_at "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
  --arg baseline_template_sha256 "$(sha256_file "$BASELINE_CONFIG")" \
  --arg sweep_template_sha256 "$(sha256_file "$SWEEP_TEMPLATE")" \
  --arg doctor_sha256 "$(sha256_file "$DOCTOR_REPORT")" \
  --arg baseline_before_sha256 "$(sha256_file "$SESSION_DIR/environment-baseline-before/result.json")" \
  --arg sweep_config_sha256 "$(sha256_file "$SWEEP_CONFIG")" \
  --arg results_sha256 "$(sha256_file "$SESSION_DIR/solutions/results.jsonl")" \
  --arg capacity_sha256 "$(sha256_file "$SESSION_DIR/solutions/capacity.json")" \
  --arg baseline_after_sha256 "$(sha256_file "$SESSION_DIR/environment-baseline-after/result.json")" \
  --arg bundle_manifest_sha256 "$(sha256_file "$BUNDLE_MANIFEST")" \
  '{
    version: 2,
    kind: "provisional_loss_pipeline_smoke",
    classification: "provisional_smoke_not_conformance_or_preset",
    status: "completed",
    generated_at: $generated_at,
    doctor_ok: $doctor[0].ok,
    baseline_before_outcome: $baseline_before[0].outcome,
    baseline_after_outcome: $baseline_after[0].outcome,
    result_cells: 4,
    point_records: ($points | length),
    point_outcomes: ($points | group_by(.outcome) | map({key: .[0].outcome, value: length}) | from_entries),
    inputs: {
      "environment-baseline-loss-provisional.json": $baseline_template_sha256,
      "sweep-scenario-loss-provisional.json": $sweep_template_sha256
    },
    artifacts: {
      "doctor.json": $doctor_sha256,
      "environment-baseline-before/result.json": $baseline_before_sha256,
      "sweep-config.json": $sweep_config_sha256,
      "solutions/results.jsonl": $results_sha256,
      "solutions/capacity.json": $capacity_sha256,
      "environment-baseline-after/result.json": $baseline_after_sha256,
      "bundle-manifest.json": $bundle_manifest_sha256
    }
  }' >"$SESSION_DIR/session-manifest.json"

python3 scripts/session_bundle_manifest.py verify "$SESSION_DIR"
chown -R "$SUDO_UID:$SUDO_GID" "$SESSION_DIR"

printf 'provisional loss pipeline smoke session: %s\n' "$SESSION_DIR"
