#!/usr/bin/env bash
# Production-profile sweep.
#
# These are the final project workloads, separate from the synthetic echo
# baseline:
#   media_relay: SFU-like unreliable media fanout.
#   game_server: authoritative game input/event fanout.
#
# Defaults compare apex against the strongest practical baselines from the
# remeasurement set. Override LIBS/RUNS/OUT from the environment if needed.

set -euo pipefail

cd "$(dirname "$0")/.."

OUT="${OUT:-results/production_profiles_$(date -u +%Y%m%dT%H%M%SZ)}"
LIBS="${LIBS:-coop_rudp,apex_rudp,litenetlib,enet,gns,raknet}"
RUNS="${RUNS:-1 2 3}"
NETEM="${NETEM:-1}"
SERVER_CPU="${SERVER_CPU:-7,15}"
CLIENT_CPU="${CLIENT_CPU:-5,6,13,14}"
# Broadcast client farms preserve the full-room fanout denominator while
# splitting local connection load across processes.
CLIENT_PROCS="${CLIENT_PROCS:-4}"
IDLE="${IDLE:-adaptive}"
ISOLATE="${ISOLATE:-systemd}"
DURATION="${DURATION:-20}"
TAIL_MS="${TAIL_MS:-500}"

mkdir -p "$OUT"

write_profiles_csv() {
  cat > "$OUT/profiles.csv" <<CSV
profile,use_case,mode,rate_r,rate_u,size,conns,duration_s,tail_ms,notes
media_relay,media_sfu_unreliable_fanout,broadcast,0,30,1000,50,$DURATION,$TAIL_MS,"50 concurrent publishers, 30Hz near-MTU media packets, full-room unreliable fanout"
game_server,authoritative_game_snapshot_event_fanout,broadcast,1,20,128,64,$DURATION,$TAIL_MS,"64-player arena, 20Hz unreliable state/input fanout plus 1Hz reliable gameplay events"
CSV
}

run_profile() {
  local profile="$1"
  local mode="$2"
  local rate_r="$3"
  local rate_u="$4"
  local size="$5"
  local conns="$6"
  local run="$7"

  echo "[$(date +%H:%M:%S)] profile=$profile conns=$conns run=$run START"
  scripts/run_phase1_quick.sh \
    --libraries="$LIBS" \
    --mode="$mode" \
    --rate-r="$rate_r" --rate-u="$rate_u" --size="$size" --conns="$conns" \
    --duration="$DURATION" --tail-ms="$TAIL_MS" \
    --idle="$IDLE" --client-procs="$CLIENT_PROCS" \
    --isolate="$ISOLATE" --server-cpu="$SERVER_CPU" --client-cpu="$CLIENT_CPU" \
    --results="$OUT/res_${profile}_r${run}.csv" \
    --diagnostics="$OUT/diag_${profile}_r${run}.csv" \
    --scenarios="$OUT/scen_${profile}_r${run}.csv" \
    --raw-dir="$OUT/raw_${profile}_r${run}" \
    --run-id="${profile}_r${run}" \
    >"$OUT/log_${profile}_r${run}.txt" 2>&1
  tail -n +2 "$OUT/res_${profile}_r${run}.csv"
  echo "[$(date +%H:%M:%S)] profile=$profile conns=$conns run=$run DONE"
}

write_profiles_csv

if [ "$NETEM" = "1" ]; then
  sudo scripts/netem.sh apply 25 5 1 100000 >"$OUT/netem_apply.txt"
  trap 'sudo scripts/netem.sh clear >"$OUT/netem_clear.txt" 2>&1 || true' EXIT
fi

for run in $RUNS; do
  run_profile media_relay broadcast 0 30 1000 50 "$run"
  run_profile game_server broadcast 1 20 128 64 "$run"
done

first_results="$(find "$OUT" -maxdepth 1 -name 'res_*.csv' | sort | head -1)"
first_scenarios="$(find "$OUT" -maxdepth 1 -name 'scen_*.csv' | sort | head -1)"

head -1 "$first_results" > "$OUT/results_all.csv"
tail -q -n +2 "$OUT"/res_*.csv >> "$OUT/results_all.csv"
head -1 "$first_scenarios" > "$OUT/scenarios_all.csv"
tail -q -n +2 "$OUT"/scen_*.csv >> "$OUT/scenarios_all.csv"

python3 scripts/aggregate_runs.py \
  --results "$OUT/results_all.csv" \
  --scenarios "$OUT/scenarios_all.csv" \
  --out "$OUT/summary.csv" \
  --min-valid 2

echo "[$(date +%H:%M:%S)] ALL DONE -> $OUT/summary.csv"
