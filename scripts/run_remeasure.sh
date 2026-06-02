#!/usr/bin/env bash
# Re-measurement sweep after the 2026-05-30 measurement-validity changes.
# v2-comparable config: mixed 50/50, size=64, 20s, warmup=2, idle=adaptive,
# netem 25/5/1 (limit=100000), isolation server=7,15 client=5,6,13,14 procs=4.
# Grid: conns {200,600,1000} x N=3 x {apex_rudp,enet,kcp,mini_rudp,gns,msquic,litenetlib}.
# Aggregated with scripts/aggregate_runs.py (median over valid runs, S1).
set -uo pipefail
cd /home/neguse/ghq/github.com/neguse/rudp-bench

OUT=results/remeasure_v3
mkdir -p "$OUT"
LIBS="apex_rudp,enet,kcp,mini_rudp,gns,msquic,litenetlib"
CONNS_LIST="200 600 1000"
RUNS="1 2 3"

for conns in $CONNS_LIST; do
  for run in $RUNS; do
    echo "[$(date +%H:%M:%S)] conns=$conns run=$run START"
    scripts/run_phase1_quick.sh \
      --libraries="$LIBS" \
      --rate-r=50 --rate-u=50 --size=64 --conns="$conns" \
      --idle=adaptive --client-procs=4 \
      --isolate=systemd --server-cpu=7,15 --client-cpu=5,6,13,14 \
      --results="$OUT/res_${conns}_r${run}.csv" \
      --diagnostics="$OUT/diag_${conns}_r${run}.csv" \
      --scenarios="$OUT/scen_${conns}_r${run}.csv" \
      --raw-dir="$OUT/raw_${conns}_r${run}" \
      --run-id="r${run}" \
      >"$OUT/log_${conns}_r${run}.txt" 2>&1 || echo "  (run_phase1 nonzero, continuing)"
    echo "[$(date +%H:%M:%S)] conns=$conns run=$run DONE"
  done
done

# Concatenate all per-(conn,run) result/scenario CSVs, then aggregate.
head -1 "$OUT"/res_200_r1.csv > "$OUT/results_all.csv"
tail -q -n +2 "$OUT"/res_*.csv >> "$OUT/results_all.csv"
head -1 "$OUT"/scen_200_r1.csv > "$OUT/scenarios_all.csv"
tail -q -n +2 "$OUT"/scen_*.csv >> "$OUT/scenarios_all.csv"
python3 scripts/aggregate_runs.py \
  --results "$OUT/results_all.csv" --scenarios "$OUT/scenarios_all.csv" \
  --out "$OUT/summary.csv" --min-valid 2
echo "[$(date +%H:%M:%S)] ALL DONE -> $OUT/summary.csv"
