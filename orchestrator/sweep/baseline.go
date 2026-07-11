package sweep

import (
	"context"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"strings"

	"github.com/neguse/rudp-bench/orchestrator/run"
)

// Baseline は block(1 回の sweep 実行)を挟む environment baseline の定義。
// 前後 2 回の baseline が drift 許容幅を外れた block は INVALID とし、SUT の
// 失敗に数えない(ADR-0002)。許容幅は pilot の観測後に凍結する値であり、
// config が宣言する。

type DriftTolerance struct {
	// |before - after| の絶対差の上限(LT delivery ratio)
	MaxDeliveryDelta float64 `json:"max_delivery_delta"`
	// max/min の比の上限(LT staleness p99)。>= 1
	MaxStalenessP99Ratio float64 `json:"max_staleness_p99_ratio"`
}

// BaselineSpec は baseline probe の実行定義。cells の transports とは独立で、
// raw probe(通常 raw_udp)を指す。
type BaselineSpec struct {
	Transport     string            `json:"transport"`
	ServerCommand run.CommandConfig `json:"server_command"`
	ClientCommand run.CommandConfig `json:"client_command"`
	ClientProcs   int               `json:"client_procs"`
	Scenario      run.ScenarioSpec  `json:"scenario"`
	TotalConns    int               `json:"total_conns"`
	Warmup        run.Duration      `json:"warmup"`
	Duration      run.Duration      `json:"duration"`
	Drain         run.Duration      `json:"drain"`
	Drift         DriftTolerance    `json:"drift"`
}

func (b BaselineSpec) validate() error {
	if !run.IsSafeName(b.Transport) {
		return fmt.Errorf("baseline.transport must be a path-safe ASCII slug")
	}
	if b.ServerCommand.Path == "" || b.ClientCommand.Path == "" {
		return fmt.Errorf("baseline server_command and client_command are required")
	}
	if b.ClientProcs <= 0 {
		return fmt.Errorf("baseline.client_procs must be > 0")
	}
	if b.Scenario.Kind != run.ScenarioEnvironmentBaseline {
		return fmt.Errorf("baseline.scenario.kind must be environment_baseline")
	}
	if err := b.Scenario.Validate(); err != nil {
		return fmt.Errorf("baseline.scenario: %w", err)
	}
	if b.TotalConns <= 0 {
		return fmt.Errorf("baseline.total_conns must be > 0")
	}
	if b.Duration.Duration <= 0 {
		return fmt.Errorf("baseline.duration must be > 0")
	}
	if b.Drift.MaxDeliveryDelta <= 0 || b.Drift.MaxDeliveryDelta > 1 {
		return fmt.Errorf("baseline.drift.max_delivery_delta must be in (0, 1]")
	}
	if b.Drift.MaxStalenessP99Ratio < 1 {
		return fmt.Errorf("baseline.drift.max_staleness_p99_ratio must be >= 1")
	}
	return nil
}

// BaselineRecord は 1 回の baseline 実行の判定入力。
type BaselineRecord struct {
	Phase          string      `json:"phase"`
	Outcome        run.Outcome `json:"outcome"`
	Verdict        string      `json:"verdict"`
	OK             bool        `json:"ok"`
	DeliveryRatio  float64     `json:"delivery_ratio"`
	StalenessP99NS uint64      `json:"staleness_p99_ns"`
	RunDir         string      `json:"run_dir"`
	RunIdentity    string      `json:"run_identity,omitempty"`
	Cause          string      `json:"cause,omitempty"`
}

// BlockBaseline は capacity.json に載る block 前後 baseline の結論。
type BlockBaseline struct {
	Before  BaselineRecord `json:"before"`
	After   *BaselineRecord `json:"after,omitempty"`
	DriftOK bool            `json:"drift_ok"`
	Cause   string          `json:"cause,omitempty"`
}

func evaluateDrift(before, after BaselineRecord, tol DriftTolerance) (bool, string) {
	for _, rec := range []BaselineRecord{before, after} {
		if !rec.OK {
			return false, fmt.Sprintf("baseline %s did not pass: %s", rec.Phase, rec.Cause)
		}
	}
	if delta := math.Abs(before.DeliveryRatio - after.DeliveryRatio); delta > tol.MaxDeliveryDelta {
		return false, fmt.Sprintf("baseline delivery drift %.6f exceeds max_delivery_delta %.6f",
			delta, tol.MaxDeliveryDelta)
	}
	lo, hi := before.StalenessP99NS, after.StalenessP99NS
	if lo > hi {
		lo, hi = hi, lo
	}
	switch {
	case hi == 0:
	case lo == 0:
		return false, "baseline staleness p99 is zero on one side only"
	case float64(hi)/float64(lo) > tol.MaxStalenessP99Ratio:
		return false, fmt.Sprintf("baseline staleness p99 ratio %.3f exceeds max_staleness_p99_ratio %.3f",
			float64(hi)/float64(lo), tol.MaxStalenessP99Ratio)
	}
	return true, ""
}

func (s *Sweep) runBaseline(ctx context.Context, phase string) (BaselineRecord, error) {
	spec := s.cfg.Baseline
	cfg := run.RunConfig{
		Transport:         spec.Transport,
		Scenario:          &spec.Scenario,
		ServerCommand:     spec.ServerCommand,
		ClientCommand:     spec.ClientCommand,
		ClientProcs:       spec.ClientProcs,
		TotalConns:        spec.TotalConns,
		Warmup:            spec.Warmup,
		Duration:          spec.Duration,
		Drain:             spec.Drain,
		StalenessPeriodNS: s.cfg.StalenessPeriodNS,
		Netem:             s.cfg.Netem,
		ServerCPUs:        s.cfg.ServerCPUs,
		ClientCPUs:        s.cfg.ClientCPUs,
		OutputDir:         filepath.Join(s.cfg.OutputDir, "baseline", phase),
	}
	cfg, err := cfg.Prepare()
	if err != nil {
		return BaselineRecord{}, fmt.Errorf("baseline %s: prepare: %w", phase, err)
	}
	runIdentity := run.ConfigIdentity(cfg)
	if s.attempts == nil {
		s.attempts = map[string]int{}
	}
	key := "baseline|" + phase + "|" + runIdentity
	s.attempts[key]++
	cfg.OutputDir = filepath.Join(s.cfg.OutputDir, "baseline", phase,
		runIdentity[:12], fmt.Sprintf("attempt-%03d", s.attempts[key]))
	fmt.Fprintf(os.Stderr, "[sweep] baseline %s running (duration=%s)...\n", phase, cfg.Duration.Duration)
	result, err := run.Run(ctx, cfg)
	if err != nil {
		return BaselineRecord{}, fmt.Errorf("baseline %s: run: %w", phase, err)
	}
	rec := BaselineRecord{
		Phase: phase, Outcome: result.Outcome, Verdict: result.Verdict,
		RunDir: cfg.OutputDir, RunIdentity: runIdentity,
	}
	rec.OK = result.Outcome == run.OutcomePass && result.Verdict == "VALID"
	var causes []string
	causes = append(causes, result.OutcomeReasons...)
	if evaluation := result.ScenarioEvaluation; evaluation != nil {
		if !evaluation.OK {
			rec.OK = false
			if evaluation.Cause != "" {
				causes = append(causes, evaluation.Cause)
			}
		}
		deliverySet := false
		for _, traffic := range evaluation.Traffic {
			if traffic.Class != run.ClassLossTolerant {
				continue
			}
			if !deliverySet || traffic.DeliveryRatio < rec.DeliveryRatio {
				rec.DeliveryRatio = traffic.DeliveryRatio
				deliverySet = true
			}
			if traffic.StalenessP99NS > rec.StalenessP99NS {
				rec.StalenessP99NS = traffic.StalenessP99NS
			}
		}
		if !deliverySet {
			rec.OK = false
			causes = append(causes, "baseline result has no loss_tolerant traffic evaluation")
		}
	} else {
		rec.OK = false
		causes = append(causes, "baseline result has no scenario evaluation")
	}
	rec.Cause = strings.Join(dedupeSorted(causes), "; ")
	fmt.Fprintf(os.Stderr, "[sweep] baseline %s → ok=%v delivery=%.6f staleness_p99=%dns %s\n",
		phase, rec.OK, rec.DeliveryRatio, rec.StalenessP99NS, rec.Cause)
	return rec, nil
}
