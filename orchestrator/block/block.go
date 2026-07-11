// Package block は E3 の1ブロック(= 1 rig セッションの完全な一周)を
// 1 コマンドで実行する: 各 sweep → boundary → metadata 記録 → tar。
// ブロックが DoE の replicate 単位(design spec: 反復は独立ブロックで数える)。
package block

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/boundary"
	"github.com/neguse/rudp-bench/orchestrator/rig"
	orun "github.com/neguse/rudp-bench/orchestrator/run"
	"github.com/neguse/rudp-bench/orchestrator/sweep"
)

type SweepEntry struct {
	Name   string `json:"name"` // block 内サブディレクトリ名(wired 等)
	Config string `json:"config"`
}

type BoundaryEntry struct {
	Name   string `json:"name"`
	Config string `json:"config"`
	// CapacityFrom: 負荷アンカーの基準にする sweep の Name。
	// ブロック内で自己完結させる(他ブロックの capacity を参照しない)
	CapacityFrom string `json:"capacity_from"`
}

type Config struct {
	MeasurementMode string          `json:"measurement_mode,omitempty"`
	Rig             string          `json:"rig"` // rig 記述ファイル(metadata として保存)
	Seed            int64           `json:"seed"`
	Sweeps          []SweepEntry    `json:"sweeps"`
	Boundaries      []BoundaryEntry `json:"boundaries"`
	OutputDir       string          `json:"output_dir"`
	// Tar: 完了時に <output_dir>.tar.gz を作る(回収用)
	Tar bool `json:"tar,omitempty"`
}

func LoadConfig(path string) (Config, error) {
	var cfg Config
	data, err := os.ReadFile(path)
	if err != nil {
		return cfg, err
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&cfg); err != nil {
		return cfg, fmt.Errorf("%s: %w", path, err)
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		return cfg, fmt.Errorf("%s: expected exactly one JSON object", path)
	}
	if cfg.OutputDir == "" || cfg.Rig == "" {
		return cfg, fmt.Errorf("output_dir and rig are required")
	}
	if len(cfg.Sweeps) == 0 {
		return cfg, fmt.Errorf("at least one sweep is required")
	}
	seenNames := map[string]bool{}
	for _, entry := range cfg.Sweeps {
		if !orun.IsSafeName(entry.Name) || entry.Config == "" || seenNames[entry.Name] {
			return cfg, fmt.Errorf("each sweep needs a unique path-safe name and config")
		}
		seenNames[entry.Name] = true
	}
	for _, entry := range cfg.Boundaries {
		if !orun.IsSafeName(entry.Name) || entry.Config == "" || seenNames[entry.Name] {
			return cfg, fmt.Errorf("each boundary needs a unique path-safe name and config")
		}
		seenNames[entry.Name] = true
	}
	if cfg.MeasurementMode == "" {
		cfg.MeasurementMode = "screening"
	}
	if cfg.MeasurementMode == "reference" {
		return cfg, fmt.Errorf("reference block execution is disabled until doctor plus pre/post baseline drift gates are integrated")
	}
	if cfg.MeasurementMode != "screening" && cfg.MeasurementMode != "pilot" {
		return cfg, fmt.Errorf("measurement_mode must be screening or pilot")
	}
	return cfg, nil
}

type metadata struct {
	Rig     rig.Rig   `json:"rig"`
	Seed    int64     `json:"seed"`
	Commit  string    `json:"commit"`
	StartAt time.Time `json:"start_at"`
	EndAt   time.Time `json:"end_at,omitempty"`
}

func gitCommit() string {
	out, err := exec.Command("git", "rev-parse", "HEAD").Output()
	if err != nil {
		return "unknown"
	}
	return strings.TrimSpace(string(out))
}

// Run はブロックを一周する。resume は各 sweep/boundary の results.jsonl に
// 委譲される(ブロックを同じ output_dir で再実行すれば続きから走る)。
func Run(ctx context.Context, cfg Config) error {
	r, err := rig.Load(cfg.Rig)
	if err != nil {
		return err
	}
	if err := os.MkdirAll(cfg.OutputDir, 0o755); err != nil {
		return err
	}

	meta := metadata{Rig: r, Seed: cfg.Seed, Commit: gitCommit(), StartAt: time.Now()}
	if err := writeMeta(cfg.OutputDir, meta); err != nil {
		return err
	}

	sweepDirs := map[string]string{}
	for _, entry := range cfg.Sweeps {
		scfg, err := sweep.LoadConfig(entry.Config)
		if err != nil {
			return fmt.Errorf("sweep %s: %w", entry.Name, err)
		}
		scfg.Seed = cfg.Seed
		scfg.OutputDir = filepath.Join(cfg.OutputDir, entry.Name)
		scfg.ServerCPUs = r.ServerCPUs
		scfg.ClientCPUs = r.ClientCPUs
		sweepDirs[entry.Name] = scfg.OutputDir

		fmt.Fprintf(os.Stderr, "[block] sweep %s → %s\n", entry.Name, scfg.OutputDir)
		s, err := sweep.New(scfg)
		if err != nil {
			return fmt.Errorf("sweep %s: %w", entry.Name, err)
		}
		_, err = s.Run(ctx)
		closeErr := s.Close()
		if err != nil {
			return fmt.Errorf("sweep %s: %w", entry.Name, err)
		}
		if closeErr != nil {
			return closeErr
		}
	}

	for _, entry := range cfg.Boundaries {
		bcfg, err := boundary.LoadConfig(entry.Config)
		if err != nil {
			return fmt.Errorf("boundary %s: %w", entry.Name, err)
		}
		bcfg.Seed = cfg.Seed
		bcfg.OutputDir = filepath.Join(cfg.OutputDir, entry.Name)
		bcfg.ServerCPUs = r.ServerCPUs
		bcfg.ClientCPUs = r.ClientCPUs
		if entry.CapacityFrom != "" {
			dir, ok := sweepDirs[entry.CapacityFrom]
			if !ok {
				return fmt.Errorf("boundary %s: capacity_from %q not in this block", entry.Name, entry.CapacityFrom)
			}
			bcfg.CapacityJSON = filepath.Join(dir, "capacity.json")
		}

		fmt.Fprintf(os.Stderr, "[block] boundary %s → %s\n", entry.Name, bcfg.OutputDir)
		b, err := boundary.New(bcfg)
		if err != nil {
			return fmt.Errorf("boundary %s: %w", entry.Name, err)
		}
		_, err = b.Run(ctx)
		closeErr := b.Close()
		if err != nil {
			return fmt.Errorf("boundary %s: %w", entry.Name, err)
		}
		if closeErr != nil {
			return closeErr
		}
	}

	meta.EndAt = time.Now()
	if err := writeMeta(cfg.OutputDir, meta); err != nil {
		return err
	}

	if cfg.Tar {
		tarPath := strings.TrimSuffix(cfg.OutputDir, "/") + ".tar.gz"
		cmd := exec.CommandContext(ctx, "tar", "czf", tarPath,
			"-C", filepath.Dir(cfg.OutputDir), filepath.Base(cfg.OutputDir))
		cmd.Stderr = os.Stderr
		if err := cmd.Run(); err != nil {
			return fmt.Errorf("tar: %w", err)
		}
		fmt.Fprintf(os.Stderr, "[block] packaged immutable run evidence: %s\n", tarPath)
	}
	return nil
}

func writeMeta(dir string, meta metadata) error {
	data, err := json.MarshalIndent(meta, "", " ")
	if err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(dir, "block.json"), append(data, '\n'), 0o644)
}
