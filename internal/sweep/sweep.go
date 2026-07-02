package sweep

import (
	"context"
	"encoding/csv"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/neguse/rudp-bench/internal/bench"
	"github.com/neguse/rudp-bench/internal/doctor"
	"github.com/neguse/rudp-bench/internal/report"
	"github.com/neguse/rudp-bench/internal/result"
	"github.com/neguse/rudp-bench/internal/runner"
)

// RunConfig holds all parameters for a sweep invocation.
type RunConfig struct {
	Root          string
	Out           string
	BuildDir      string
	Libs          []string
	Profiles      []bench.Profile
	Runs          []string // e.g. ["1", "2", "3"]
	Duration      int
	TailMs        int
	Idle          string
	Isolate       string
	ServerCPU     string
	ClientCPU     string
	Netem         bool
	NetemArgs     string
	MinValid      int
	MinDelivery   float64
	Build         bool
	Publish       bool
	NoBuild       bool
	NoPublish     bool
	DryRun        bool
	Plan          bool
	Resume        bool
	Adaptive      bool
	Prior         map[string]result.PriorCapacity
	Jobs          int
	CMake         string
	LitenetlibBin string
}

// Run executes the full sweep: doctor check, optional build, netem/isolation
// setup, then iterates profiles x conns x runs calling runner.Exec for each
// point. After all points are collected, it combines CSVs, writes capacity,
// renders a report, and optionally publishes.
func Run(ctx context.Context, cfg RunConfig) error {
	// 1. Doctor check
	checks := doctor.Check(cfg.BuildDir, cfg.ServerCPU, cfg.ClientCPU)
	if doctor.HasFatal(checks) && !cfg.DryRun {
		return fmt.Errorf("doctor check found fatal issues; aborting")
	}

	// 2. Build
	if cfg.Build && !cfg.NoBuild && !cfg.DryRun {
		jobs := cfg.Jobs
		if jobs <= 0 {
			jobs = 1
		}
		cmd := exec.CommandContext(ctx, "cmake", "--build", cfg.BuildDir,
			"--target", "rudp-bench", "-j", strconv.Itoa(jobs))
		cmd.Stdout = os.Stdout
		cmd.Stderr = os.Stderr
		if err := cmd.Run(); err != nil {
			return fmt.Errorf("cmake build failed: %w", err)
		}
	}

	// 3. Create output dir
	if err := os.MkdirAll(cfg.Out, 0755); err != nil {
		return fmt.Errorf("creating output dir: %w", err)
	}

	// 4. Write profiles.csv
	if err := writeProfiles(filepath.Join(cfg.Out, "profiles.csv"), cfg.Profiles); err != nil {
		return fmt.Errorf("writing profiles.csv: %w", err)
	}

	// Resume validation and manifest
	if cfg.Resume {
		if err := ValidateResume(cfg.Out, cfg); err != nil {
			return fmt.Errorf("resume validation failed: %w", err)
		}
	}
	if err := WriteManifest(cfg.Out, cfg); err != nil {
		return fmt.Errorf("writing manifest: %w", err)
	}
	completed := CompletedPoints(cfg.Out)

	// Port assignment: build a map of lib -> port
	portMap := make(map[string]int, len(cfg.Libs))
	for i, lib := range cfg.Libs {
		portMap[lib] = runner.DefaultPort(i)
	}

	// Resolve isolate mode
	isolateMode := runner.IsolateTaskset
	if cfg.Isolate == "systemd" {
		isolateMode = runner.IsolateSystemd
	}

	// Capacity tracker
	tracker := result.NewCapacityTracker()

	// 5. 前回実行の残留環境を冪等にクリアする。teardown は defer でしか走らない
	// ため、前回が異常終了 (os.Exit / kill) だと netem qdisc や CPU 隔離が残る。
	// エラーは warning 扱いで続行する。
	if !cfg.DryRun {
		if cfg.Isolate == "systemd" {
			if err := runner.IsolateTeardown(); err != nil {
				fmt.Fprintf(os.Stderr, "warning: pre-run isolate teardown failed: %v\n", err)
			}
		}
		if cfg.Netem {
			// ログは残さない（qdisc 未設定時の del 失敗は正常系）。
			runner.ClearNetem("")
		}
	}

	// 5b. Isolate setup
	if cfg.Isolate == "systemd" && !cfg.DryRun {
		if err := runner.IsolateSetup(); err != nil {
			return fmt.Errorf("isolate setup: %w", err)
		}
		defer runner.IsolateTeardown()
	}

	// 6. Netem apply
	if cfg.Netem && !cfg.DryRun {
		if err := runner.ApplyNetem(cfg.NetemArgs, cfg.Out); err != nil {
			return fmt.Errorf("netem apply: %w", err)
		}
		defer runner.ClearNetem(cfg.Out)
	}

	// 7. Sweep loop
	// adaptive skip で run 数を 1 に減らした (library, scenario_id) の集合。
	// 集計時の minValid 緩和を skip した lib のグループのみに限定するために使う。
	adaptiveSkipped := make(map[[2]string]bool)
	minValidFor := func(lib, scenarioID string) int {
		if adaptiveSkipped[[2]string{lib, scenarioID}] {
			return 1
		}
		return cfg.MinValid
	}

	for _, profile := range cfg.Profiles {
		if len(profile.Conns) == 0 {
			fmt.Printf("profile=%s skipped (empty conns schedule)\n", profile.Name)
			continue
		}

		// Filter libs unsupported for this profile
		var active []string
		for _, lib := range cfg.Libs {
			reason := unsupportedProfileReason(profile, lib)
			if reason != "" {
				tracker.MarkStop(profile, lib, profile.Conns[0], reason)
			} else {
				active = append(active, lib)
			}
		}

		for _, conns := range profile.Conns {
			if len(active) == 0 {
				break
			}

			// Check unsupported conns
			var unsupNow []string
			for _, lib := range active {
				if unsupportedConns(lib, conns) {
					unsupNow = append(unsupNow, lib)
				}
			}
			for _, lib := range unsupNow {
				tracker.MarkStop(profile, lib, conns, "unsupported_conns")
			}
			if len(unsupNow) > 0 {
				unsupSet := toSet(unsupNow)
				var remaining []string
				for _, lib := range active {
					if !unsupSet[lib] {
						remaining = append(remaining, lib)
					}
				}
				active = remaining
				fmt.Printf("  profile=%s unsupported_conns=%s remaining=%s\n",
					profile.Name, strings.Join(unsupNow, ","), joinOrDash(active))
			}
			if len(active) == 0 {
				break
			}

			clientProcs := profile.ClientProcs
			if clientProcs > conns {
				clientProcs = conns
			}
			if clientProcs < 1 {
				clientProcs = 1
			}

			// lib-outer / run-inner: each lib runs its N runs consecutively.
			// Adaptive mode can break early after run=1 if prior+current both OK.
			for _, lib := range active {
				for ri, r := range cfg.Runs {
					runID := fmt.Sprintf("%s_c%d_r%s", profile.Name, conns, r)
					scenarioID := fmt.Sprintf("%s_r%d_u%d_%d_%d_%s_0_%s",
						lib, profile.RateR, profile.RateU,
						profile.Size, conns, profile.Mode, cfg.Idle)
					resultsPath := filepath.Join(cfg.Out, fmt.Sprintf("res_%s.csv", runID))

					if cfg.Resume && completed[CompletedKey(runID, lib)] {
						fmt.Printf("[%s] %s %s c=%d run=%s SKIP (completed)\n",
							time.Now().Format("15:04:05"), profile.Name, lib, conns, r)
						// resume 時も run=1 完了直後と同じ adaptive skip 判定を行う。
						// 前回 adaptive skip した lib で残り run を無駄に再実行しないため。
						if cfg.Adaptive && ri == 0 && len(cfg.Runs) > 1 && !cfg.DryRun &&
							result.PriorWasOK(cfg.Prior, profile.Name, lib, conns) &&
							checkRun1Delivery(resultsPath, lib, scenarioID, cfg.MinDelivery) {
							fmt.Printf("  %s %s c=%d adaptive: prior+run1 OK, skip runs %s\n",
								profile.Name, lib, conns, strings.Join(cfg.Runs[1:], ","))
							adaptiveSkipped[[2]string{lib, scenarioID}] = true
							break
						}
						continue
					}

					fmt.Printf("[%s] %s %s c=%d run=%s START\n",
						time.Now().Format("15:04:05"), profile.Name, lib, conns, r)

					execOpts := runner.ExecOpts{
						BuildDir:      cfg.BuildDir,
						Library:       lib,
						Mode:          profile.Mode,
						RateR:         profile.RateR,
						RateU:         profile.RateU,
						Size:          profile.Size,
						Conns:         conns,
						Duration:      cfg.Duration,
						TailMs:        cfg.TailMs,
						Idle:          cfg.Idle,
						ClientProcs:   clientProcs,
						Isolate:       isolateMode,
						ServerCPU:     cfg.ServerCPU,
						ClientCPU:     cfg.ClientCPU,
						Port:          portMap[lib],
						OutDir:        filepath.Join(cfg.Out, fmt.Sprintf("raw_%s", runID)),
						RunID:         runID,
						ScenarioID:    scenarioID,
						Profile:       profile.Name,
						RampUpMs:      -1,
						LitenetlibBin: cfg.LitenetlibBin,
						DryRun:        cfg.DryRun,
						Results:       resultsPath,
						Diagnostics:   filepath.Join(cfg.Out, fmt.Sprintf("diag_%s.csv", runID)),
						Scenarios:     filepath.Join(cfg.Out, fmt.Sprintf("scen_%s.csv", runID)),
					}

					// 再実行時に result.Append の追記で同一 (run_id, library) の
					// 行が重複しないよう、既存行を除去してから Exec する。
					// diagnostics には library 列がないため scenario_id でも照合する
					// (scenario_id は lib を含み lib ごとに一意)。
					if !cfg.DryRun {
						staleMatch := func(row map[string]string) bool {
							return row["run_id"] == runID &&
								(row["library"] == lib || row["scenario_id"] == scenarioID)
						}
						for _, p := range []string{execOpts.Results, execOpts.Diagnostics, execOpts.Scenarios} {
							removed, err := result.RemoveRows(p, staleMatch)
							if err != nil {
								fmt.Fprintf(os.Stderr, "warning: removing stale rows from %s: %v\n", p, err)
							} else if removed > 0 {
								fmt.Printf("  removed %d stale row(s) for %s from %s\n",
									removed, lib, filepath.Base(p))
							}
						}
					}

					if err := runner.Exec(ctx, execOpts); err != nil {
						fmt.Fprintf(os.Stderr,
							"runner returned error for lib=%s profile=%s conns=%d run=%s: %v\n",
							lib, profile.Name, conns, r, err)
					}

					// Adaptive: after run=1, check if this lib can skip remaining runs
					if cfg.Adaptive && ri == 0 && len(cfg.Runs) > 1 && !cfg.DryRun {
						if result.PriorWasOK(cfg.Prior, profile.Name, lib, conns) {
							if run1OK := checkRun1Delivery(execOpts.Results, lib, scenarioID, cfg.MinDelivery); run1OK {
								fmt.Printf("  %s %s c=%d adaptive: prior+run1 OK, skip runs %s\n",
									profile.Name, lib, conns, strings.Join(cfg.Runs[1:], ","))
								adaptiveSkipped[[2]string{lib, scenarioID}] = true
								break
							}
						}
					}
				}
			}

			if cfg.DryRun {
				continue
			}

			// After all libs complete their runs for this conns point: combine and aggregate.
			// adaptive skip した lib は N=1 でも valid になるよう、
			// minValidFor がその lib のグループだけ min_valid=1 を返す。
			summaryPath, err := connSummary(cfg.Out, profile.Name, conns, minValidFor)
			if err != nil {
				fmt.Fprintf(os.Stderr, "connSummary failed for profile=%s conns=%d: %v\n",
					profile.Name, conns, err)
				continue
			}

			summaryRows, err := result.ReadCSVRows(summaryPath)
			if err != nil {
				fmt.Fprintf(os.Stderr, "reading summary %s: %v\n", summaryPath, err)
				continue
			}

			rowsByLib := make(map[string]map[string]string)
			for _, row := range summaryRows {
				rowsByLib[row["library"]] = row
			}

			broken := tracker.UpdateRows(profile, active, rowsByLib, conns, cfg.MinDelivery)

			// Print status for each active lib
			for _, row := range summaryRows {
				lib := row["library"]
				if containsStr(active, lib) {
					reason := sweepStopReason(row, cfg.MinDelivery)
					fmt.Printf("  %s %s c=%d delivery=%s cpu=%s stop=%s\n",
						profile.Name, lib, conns,
						row["delivery_ratio_median"],
						row["server_cpu_pct_median"],
						reason)
				}
			}

			if len(broken) > 0 {
				brokenSet := toSet(broken)
				var remaining []string
				for _, lib := range active {
					if !brokenSet[lib] {
						remaining = append(remaining, lib)
					}
				}
				active = remaining
				fmt.Printf("  profile=%s broken=%s remaining=%s\n",
					profile.Name, strings.Join(broken, ","), joinOrDash(active))
			}
		}
	}

	if cfg.DryRun {
		return nil
	}

	// 8. Combine all
	if err := combineAll(cfg.Out, minValidFor); err != nil {
		return fmt.Errorf("combineAll: %w", err)
	}

	// 9. Write capacity.csv
	if err := tracker.WriteCSV(filepath.Join(cfg.Out, "capacity.csv")); err != nil {
		return fmt.Errorf("writing capacity.csv: %w", err)
	}
	fmt.Printf("wrote %s\n", filepath.Join(cfg.Out, "capacity.csv"))
	fmt.Printf("wrote %s\n", filepath.Join(cfg.Out, "summary.csv"))

	// 10. Report
	if err := report.Render(cfg.Out, filepath.Join(cfg.Out, "report.md"), "", ""); err != nil {
		return fmt.Errorf("render report: %w", err)
	}

	// 11. Publish
	if cfg.Publish && !cfg.NoPublish {
		dest := report.DefaultPublishDest(cfg.Root)
		currentMD := filepath.Join(cfg.Root, "docs", "measurements", "current.md")
		if err := report.Publish(cfg.Out, dest, currentMD, cfg.Root); err != nil {
			return fmt.Errorf("publish: %w", err)
		}
	}

	return nil
}

// writeProfiles writes the profile definitions to a CSV file.
func writeProfiles(path string, profiles []bench.Profile) error {
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		return err
	}
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	defer f.Close()
	w := csv.NewWriter(f)
	w.UseCRLF = false
	if err := w.Write(bench.ProfileFields); err != nil {
		return err
	}
	for _, p := range profiles {
		connsStrs := make([]string, len(p.Conns))
		for i, c := range p.Conns {
			connsStrs[i] = strconv.Itoa(c)
		}
		record := []string{
			p.Name,
			p.UseCase,
			p.Mode,
			strconv.Itoa(p.RateR),
			strconv.Itoa(p.RateU),
			strconv.Itoa(p.Size),
			strings.Join(connsStrs, " "),
			strconv.Itoa(p.ClientProcs),
			p.Notes,
		}
		if err := w.Write(record); err != nil {
			return err
		}
	}
	w.Flush()
	return w.Error()
}

// connSummary combines per-run CSVs for a single (profile, conns) point
// into combined results/scenarios CSVs, then aggregates into a summary.
func connSummary(outDir, profileName string, conns int, minValid result.MinValidPolicy) (string, error) {
	resPattern := filepath.Join(outDir, fmt.Sprintf("res_%s_c%d_r*.csv", profileName, conns))
	scenPattern := filepath.Join(outDir, fmt.Sprintf("scen_%s_c%d_r*.csv", profileName, conns))

	resultsPath := filepath.Join(outDir, fmt.Sprintf("results_%s_c%d.csv", profileName, conns))
	scenariosPath := filepath.Join(outDir, fmt.Sprintf("scenarios_%s_c%d.csv", profileName, conns))
	summaryPath := filepath.Join(outDir, fmt.Sprintf("summary_%s_c%d.csv", profileName, conns))

	if _, err := result.CombineCSV([]string{resPattern}, resultsPath); err != nil {
		return "", fmt.Errorf("combining results: %w", err)
	}
	if _, err := result.CombineCSV([]string{scenPattern}, scenariosPath); err != nil {
		return "", fmt.Errorf("combining scenarios: %w", err)
	}
	if err := result.Aggregate(resultsPath, scenariosPath, summaryPath, minValid); err != nil {
		return "", fmt.Errorf("aggregating: %w", err)
	}
	return summaryPath, nil
}

// combineAll combines all per-point result/scenario CSVs into combined files
// and aggregates into summary.csv.
func combineAll(outDir string, minValid result.MinValidPolicy) error {
	resPattern := filepath.Join(outDir, "res_*_c*_r*.csv")
	scenPattern := filepath.Join(outDir, "scen_*_c*_r*.csv")

	resultsAll := filepath.Join(outDir, "results_all.csv")
	scenariosAll := filepath.Join(outDir, "scenarios_all.csv")
	summaryPath := filepath.Join(outDir, "summary.csv")

	if _, err := result.CombineCSV([]string{resPattern}, resultsAll); err != nil {
		return fmt.Errorf("combining all results: %w", err)
	}
	if _, err := result.CombineCSV([]string{scenPattern}, scenariosAll); err != nil {
		return fmt.Errorf("combining all scenarios: %w", err)
	}
	if err := result.Aggregate(resultsAll, scenariosAll, summaryPath, minValid); err != nil {
		return fmt.Errorf("aggregating all: %w", err)
	}
	return nil
}

// unsupportedProfileReason returns a reason string if a library does not
// support the channels required by a profile, or "" if supported.
func unsupportedProfileReason(profile bench.Profile, lib string) string {
	type chanRate struct {
		channel string
		rate    int
	}
	for _, cr := range []chanRate{{"r", profile.RateR}, {"u", profile.RateU}} {
		if cr.rate <= 0 {
			continue
		}
		if !bench.SupportsReliability(lib, cr.channel) {
			if cr.channel == "r" {
				return "unsupported_reliable"
			}
			return "unsupported_unreliable"
		}
		maxPayload, ok := bench.MaxPayloadBytes(lib, cr.channel)
		if ok && profile.Size > maxPayload {
			return "unsupported_payload"
		}
	}
	return ""
}

// unsupportedConns returns true if the library's max connection limit is
// exceeded by conns.
func unsupportedConns(lib string, conns int) bool {
	maxConns, ok := bench.MaxConnections(lib)
	return ok && conns > maxConns
}

// sweepStopReason evaluates a summary row and returns a stop reason string.
// Returns "ok" if the row passes all gates.
func sweepStopReason(row map[string]string, minDelivery float64) string {
	if row == nil {
		return "missing_summary"
	}
	if row["valid"] != "1" {
		note := row["note"]
		if note == "" {
			note = fmt.Sprintf("valid_runs=%s/%s", row["n_valid"], row["n_total"])
		}
		if strings.HasPrefix(note, "unsupported_") {
			return note
		}
		return "aggregate_invalid:" + note
	}
	deliveryStr := row["delivery_ratio_median"]
	if deliveryStr == "" {
		return "missing_delivery"
	}
	delivery, err := strconv.ParseFloat(deliveryStr, 64)
	if err != nil {
		return "missing_delivery"
	}
	if delivery < minDelivery {
		return fmt.Sprintf("delivery<%.2f", minDelivery)
	}
	return "ok"
}

// toSet converts a string slice to a set (map[string]bool).
func toSet(ss []string) map[string]bool {
	m := make(map[string]bool, len(ss))
	for _, s := range ss {
		m[s] = true
	}
	return m
}

// containsStr checks if a string slice contains a value.
func containsStr(ss []string, s string) bool {
	for _, v := range ss {
		if v == s {
			return true
		}
	}
	return false
}

// checkRun1Delivery reads the per-run result CSV after run=1 and returns true
// if the library's delivery_ratio >= minDelivery and the run is valid.
func checkRun1Delivery(resultsCSV, lib, scenarioID string, minDelivery float64) bool {
	rows, err := result.ReadCSVRows(resultsCSV)
	if err != nil || len(rows) == 0 {
		return false
	}
	for _, r := range rows {
		if r["library"] == lib && r["scenario_id"] == scenarioID {
			if r["valid"] != "1" {
				return false
			}
			delivery, ok := result.FloatOrNone(r["delivery_ratio"])
			if !ok {
				return false
			}
			return delivery >= minDelivery
		}
	}
	return false
}

// joinOrDash returns the joined string or "-" if empty.
func joinOrDash(ss []string) string {
	if len(ss) == 0 {
		return "-"
	}
	return strings.Join(ss, ",")
}
