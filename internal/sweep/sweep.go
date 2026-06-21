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

	// 5. Isolate setup
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

			// Run each repetition: for each run, execute all active libs
			for _, r := range cfg.Runs {
				runID := fmt.Sprintf("%s_c%d_r%s", profile.Name, conns, r)

				// Resume: skip completed points
				if cfg.Resume && completed[runID] {
					fmt.Printf("[%s] profile=%s conns=%d run=%s SKIP (completed)\n",
						time.Now().Format("15:04:05"), profile.Name, conns, r)
					continue
				}

				clientProcs := profile.ClientProcs
				if clientProcs > conns {
					clientProcs = conns
				}
				if clientProcs < 1 {
					clientProcs = 1
				}

				fmt.Printf("[%s] profile=%s conns=%d run=%s libs=%s START\n",
					time.Now().Format("15:04:05"), profile.Name, conns, r,
					strings.Join(active, ","))

				for _, lib := range active {
					scenarioID := fmt.Sprintf("%s_r%d_u%d_%d_%d_%s_0_%s",
						lib, profile.RateR, profile.RateU,
						profile.Size, conns, profile.Mode, cfg.Idle)

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
						RampUpMs:      -1,
						LitenetlibBin: cfg.LitenetlibBin,
						DryRun:        cfg.DryRun,
						Results:       filepath.Join(cfg.Out, fmt.Sprintf("res_%s.csv", runID)),
						Diagnostics:   filepath.Join(cfg.Out, fmt.Sprintf("diag_%s.csv", runID)),
						Scenarios:     filepath.Join(cfg.Out, fmt.Sprintf("scen_%s.csv", runID)),
					}

					if err := runner.Exec(ctx, execOpts); err != nil {
						fmt.Fprintf(os.Stderr,
							"runner returned error for lib=%s profile=%s conns=%d run=%s: %v\n",
							lib, profile.Name, conns, r, err)
					}
				}

				fmt.Printf("[%s] profile=%s conns=%d run=%s DONE\n",
					time.Now().Format("15:04:05"), profile.Name, conns, r)
			}

			if cfg.DryRun {
				continue
			}

			// After all runs for this conns point: combine and aggregate
			summaryPath, err := connSummary(cfg.Out, profile.Name, conns, cfg.MinValid)
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
	if err := combineAll(cfg.Out, cfg.MinValid); err != nil {
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
func connSummary(outDir, profileName string, conns, minValid int) (string, error) {
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
func combineAll(outDir string, minValid int) error {
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

// joinOrDash returns the joined string or "-" if empty.
func joinOrDash(ss []string) string {
	if len(ss) == 0 {
		return "-"
	}
	return strings.Join(ss, ",")
}
