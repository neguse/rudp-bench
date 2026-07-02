package sweep

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/neguse/rudp-bench/internal/result"
)

// Manifest records sweep parameters so a resumed run can verify consistency.
type Manifest struct {
	StartTime string   `json:"start_time"`
	Libs      []string `json:"libs"`
	Profiles  []string `json:"profiles"`
	Runs      []string `json:"runs"`
	Duration  int      `json:"duration"`
	TailMs    int      `json:"tail_ms"`
	Netem     bool     `json:"netem"`
	NetemArgs string   `json:"netem_args"`
	Isolate   string   `json:"isolate"`
}

// WriteManifest writes a manifest.json to the output directory.
func WriteManifest(outDir string, cfg RunConfig) error {
	manifestPath := filepath.Join(outDir, "manifest.json")

	// If manifest already exists (resume case), don't overwrite.
	if _, err := os.Stat(manifestPath); err == nil {
		return nil
	}

	profileNames := make([]string, len(cfg.Profiles))
	for i, p := range cfg.Profiles {
		profileNames[i] = p.Name
	}

	m := Manifest{
		StartTime: time.Now().UTC().Format(time.RFC3339),
		Libs:      cfg.Libs,
		Profiles:  profileNames,
		Runs:      cfg.Runs,
		Duration:  cfg.Duration,
		TailMs:    cfg.TailMs,
		Netem:     cfg.Netem,
		NetemArgs: cfg.NetemArgs,
		Isolate:   cfg.Isolate,
	}

	data, err := json.MarshalIndent(m, "", "  ")
	if err != nil {
		return fmt.Errorf("marshaling manifest: %w", err)
	}
	data = append(data, '\n')
	return os.WriteFile(manifestPath, data, 0644)
}

// ReadManifest reads and parses manifest.json from the output directory.
func ReadManifest(outDir string) (*Manifest, error) {
	data, err := os.ReadFile(filepath.Join(outDir, "manifest.json"))
	if err != nil {
		return nil, err
	}
	var m Manifest
	if err := json.Unmarshal(data, &m); err != nil {
		return nil, fmt.Errorf("parsing manifest: %w", err)
	}
	return &m, nil
}

// ValidateResume reads the existing manifest and compares it with the current
// config. Returns an error if any critical parameter has changed.
func ValidateResume(outDir string, cfg RunConfig) error {
	m, err := ReadManifest(outDir)
	if err != nil {
		if os.IsNotExist(err) {
			// No manifest means this is a fresh run, not a resume conflict.
			return nil
		}
		return fmt.Errorf("reading manifest for resume: %w", err)
	}

	profileNames := make([]string, len(cfg.Profiles))
	for i, p := range cfg.Profiles {
		profileNames[i] = p.Name
	}

	var mismatches []string

	if !sliceEqual(m.Libs, cfg.Libs) {
		mismatches = append(mismatches,
			fmt.Sprintf("libs: manifest=%s current=%s",
				strings.Join(m.Libs, ","), strings.Join(cfg.Libs, ",")))
	}
	if !sliceEqual(m.Profiles, profileNames) {
		mismatches = append(mismatches,
			fmt.Sprintf("profiles: manifest=%s current=%s",
				strings.Join(m.Profiles, ","), strings.Join(profileNames, ",")))
	}
	if !sliceEqual(m.Runs, cfg.Runs) {
		mismatches = append(mismatches,
			fmt.Sprintf("runs: manifest=%s current=%s",
				strings.Join(m.Runs, ","), strings.Join(cfg.Runs, ",")))
	}
	if m.Duration != cfg.Duration {
		mismatches = append(mismatches,
			fmt.Sprintf("duration: manifest=%d current=%d", m.Duration, cfg.Duration))
	}
	if m.TailMs != cfg.TailMs {
		mismatches = append(mismatches,
			fmt.Sprintf("tail_ms: manifest=%d current=%d", m.TailMs, cfg.TailMs))
	}
	if m.Netem != cfg.Netem {
		mismatches = append(mismatches,
			fmt.Sprintf("netem: manifest=%v current=%v", m.Netem, cfg.Netem))
	}
	if m.NetemArgs != cfg.NetemArgs {
		mismatches = append(mismatches,
			fmt.Sprintf("netem_args: manifest=%q current=%q", m.NetemArgs, cfg.NetemArgs))
	}
	if m.Isolate != cfg.Isolate {
		mismatches = append(mismatches,
			fmt.Sprintf("isolate: manifest=%s current=%s", m.Isolate, cfg.Isolate))
	}

	if len(mismatches) > 0 {
		return fmt.Errorf("resume config mismatch:\n  %s", strings.Join(mismatches, "\n  "))
	}
	return nil
}

// CompletedKey builds the lookup key used by CompletedPoints and the sweep
// loop for a (run_id, library) point.
func CompletedKey(runID, lib string) string {
	return runID + "/" + lib
}

// CompletedPoints scans the output directory for existing res_*_c*_r*.csv
// files and returns a set of completed (run_id, library) points keyed by
// CompletedKey (e.g. "media_relay_c50_r1/enet").
// ファイルの存在だけでなく、CSV 内に (run_id, library) の行が実在することを
// 完了条件とする（途中で落ちた run の空ファイルを完了扱いしないため）。
func CompletedPoints(outDir string) map[string]bool {
	completed := make(map[string]bool)
	matches, err := filepath.Glob(filepath.Join(outDir, "res_*_c*_r*.csv"))
	if err != nil {
		return completed
	}
	for _, path := range matches {
		rows, err := result.ReadCSVRows(path)
		if err != nil {
			continue
		}
		for _, row := range rows {
			runID := row["run_id"]
			lib := row["library"]
			if runID == "" || lib == "" {
				continue
			}
			completed[CompletedKey(runID, lib)] = true
		}
	}
	return completed
}

// sliceEqual reports whether two string slices are identical.
func sliceEqual(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}
