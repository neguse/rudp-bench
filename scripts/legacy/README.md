# Legacy Scripts

These scripts were useful for one-off investigations or older measurement
passes, but they are not part of the current canonical workflow.

Current entrypoints remain in `scripts/`:

- `run_canonical_tests.sh`
- `run_final_saturation_profiles.py`
- `run_phase1_quick.sh`
- `run_phase1.sh` (kept until the canonical runner is replaced)
- `reduce_result.py`, `aggregate_runs.py`, `combine_clients.py`
- `render_canonical_report.py`, `publish_canonical_result.py`
- `netem.sh`, `set_loss.sh`, `bench_isolate.sh`

Do not add new automation here. Replace old flows with the canonical runner
instead.
