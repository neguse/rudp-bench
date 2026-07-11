#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

ORCHESTRATOR="${ORCHESTRATOR:-build-v2/orchestrator}"
SESSION="${SESSION:-$(date -u +%Y%m%dT%H%M%SZ)-$$}"
SESSION_DIR="results-v2/conformance-sessions/$SESSION"
DOCTOR_REPORT="$SESSION_DIR/doctor.json"
SWEEP_CONFIG="$SESSION_DIR/sweep-config.json"
BUNDLE_MANIFEST="$SESSION_DIR/bundle-manifest.json"

mkdir -p "$SESSION_DIR"
CALIBRATION_DIR="$SESSION_DIR/duration-invariance" \
  ./calibration/duration_invariance.sh

doctor_rc=0
"$ORCHESTRATOR" doctor \
  -rig orchestrator/rigs/home.json \
  -output "$DOCTOR_REPORT" || doctor_rc=$?
if [[ "$doctor_rc" -ne 0 && "$doctor_rc" -ne 2 ]]; then
  exit "$doctor_rc"
fi

# A doctor FAIL is allowed here because this is an implementation smoke,
# not a reference performance campaign. The report preserves that distinction.
JQ_DOCTOR_REPORT="$DOCTOR_REPORT" jq \
  '.doctor_report = env.JQ_DOCTOR_REPORT' \
  orchestrator/examples/sweep-scenario-conformance-local.json >"$SWEEP_CONFIG"

"$ORCHESTRATOR" run \
  -config orchestrator/examples/local-baseline-rawudp.json \
  -output-dir "$SESSION_DIR/environment-baseline"

"$ORCHESTRATOR" sweep \
  -config "$SWEEP_CONFIG" \
  -output-dir "$SESSION_DIR/solutions"

jq -e '
  (.cells | length) == 12 and
  all(.cells[];
    .capacity == 3 and .range_limited == true and
    (.censored // false) == false and (.below_range // false) == false)
' "$SESSION_DIR/solutions/capacity.json" >/dev/null

jq -s -e '
  length == 12 and
  all(.[]; .outcome == "PASS" and .verdict == "VALID" and .attempt == 1) and
  ([.[].acquisition_id] | unique | length) == 12
' "$SESSION_DIR/solutions/results.jsonl" >/dev/null

python3 scripts/session_bundle_manifest.py create "$SESSION_DIR"

sha256_file() {
  sha256sum "$1" | awk '{print $1}'
}

jq -n \
  --arg generated_at "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
  --arg calibration_sha256 "$(sha256_file "$SESSION_DIR/duration-invariance/summary.json")" \
  --arg doctor_sha256 "$(sha256_file "$DOCTOR_REPORT")" \
  --arg baseline_sha256 "$(sha256_file "$SESSION_DIR/environment-baseline/result.json")" \
  --arg sweep_config_sha256 "$(sha256_file "$SWEEP_CONFIG")" \
  --arg results_sha256 "$(sha256_file "$SESSION_DIR/solutions/results.jsonl")" \
  --arg capacity_sha256 "$(sha256_file "$SESSION_DIR/solutions/capacity.json")" \
  --arg bundle_manifest_sha256 "$(sha256_file "$BUNDLE_MANIFEST")" \
  '{
    version: 2,
    kind: "no_loss_scenario_smoke",
    status: "completed",
    generated_at: $generated_at,
    result_cells: 12,
    artifacts: {
      "duration-invariance/summary.json": $calibration_sha256,
      "doctor.json": $doctor_sha256,
      "environment-baseline/result.json": $baseline_sha256,
      "sweep-config.json": $sweep_config_sha256,
      "solutions/results.jsonl": $results_sha256,
      "solutions/capacity.json": $capacity_sha256,
      "bundle-manifest.json": $bundle_manifest_sha256
    }
  }' >"$SESSION_DIR/session-manifest.json"

python3 scripts/session_bundle_manifest.py verify "$SESSION_DIR"

printf 'no-loss scenario smoke session: %s\n' "$SESSION_DIR"
