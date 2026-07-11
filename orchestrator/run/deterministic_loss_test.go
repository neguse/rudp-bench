package run

import (
	"strings"
	"testing"

	"github.com/neguse/rudp-bench/orchestrator/control"
	"github.com/neguse/rudp-bench/orchestrator/netops"
	"github.com/neguse/rudp-bench/orchestrator/netops/losstrace"
)

func validDeterministicLossFixture(t *testing.T) (RunConfig, *control.Result, *NetemLossEvidence) {
	t.Helper()
	const traceBits = 1 << 12
	egress := netops.Netem{DelayMS: 25, LossPercent: 1, LossSeed: 42, TraceBits: traceBits}
	cfg := RunConfig{Netem: &NetemRegime{ServerEgress: egress, ClientEgress: egress}}
	schedule := control.ScheduleMessage{StartAtNS: 1_000_000, StopAtNS: 60_001_000_000}
	words, realized, err := losstrace.Generate(egress.LossSeed, egress.LossPercent, egress.LossBurstLen, traceBits)
	if err != nil {
		t.Fatal(err)
	}
	drops, err := losstrace.CountDropsInRange(words, 100, 5100)
	if err != nil {
		t.Fatal(err)
	}
	if drops == 0 {
		t.Fatal("fixture trace has no drops in range; pick another seed")
	}
	pair := cfg.Netem.pairSpec()
	record := func(ns, dev string) *DeterministicLossDirection {
		return &DeterministicLossDirection{
			Namespace: ns, Dev: dev, TraceBits: traceBits,
			CounterBefore: 100, CounterAfter: 5100, Packets: 5000,
			ExpectedDrops: drops, RealizedLossPct: realized, TraceSHA256: HashValue(words),
		}
	}
	evidence := &NetemLossEvidence{
		Version: 1, Mode: lossEvidenceModeDeterministicTrace, Supported: true,
		Scope: lossEvidenceScopeEffectiveInner, Schedule: schedule,
		Deterministic: &DeterministicLossEvidence{
			CaptureBeforeStartNS:  schedule.StartAtNS + 10,
			CaptureBeforeFinishNS: schedule.StartAtNS + 20,
			CaptureAfterStartNS:   schedule.StopAtNS - 100,
			CaptureAfterFinishNS:  schedule.StopAtNS - 90,
			ServerEgress:          record(pair.ServerNS, pair.ServerVeth),
			ClientEgress:          record(pair.ClientNS, pair.ClientVeth),
		},
	}
	return cfg, &control.Result{Schedule: schedule}, evidence
}

func TestValidateDeterministicLossEvidenceAcceptsKnownPacketAccounting(t *testing.T) {
	cfg, controlResult, evidence := validDeterministicLossFixture(t)
	if reasons := ValidateNetemLossEvidence(&cfg, controlResult, evidence); len(reasons) != 0 {
		t.Fatalf("valid deterministic evidence rejected: %v", reasons)
	}
}

func TestValidateDeterministicLossEvidenceRejectsTampering(t *testing.T) {
	for name, mutate := range map[string]func(*RunConfig, *NetemLossEvidence){
		"missing-evidence": func(cfg *RunConfig, e *NetemLossEvidence) { e.Deterministic = nil },
		"zero-drops": func(cfg *RunConfig, e *NetemLossEvidence) {
			e.Deterministic.ServerEgress.ExpectedDrops = 0
		},
		"tampered-hash": func(cfg *RunConfig, e *NetemLossEvidence) {
			e.Deterministic.ServerEgress.TraceSHA256 = strings.Repeat("0", 64)
		},
		"out-of-window": func(cfg *RunConfig, e *NetemLossEvidence) {
			e.Deterministic.CaptureAfterFinishNS = e.Schedule.StopAtNS + 1
		},
		"backwards-counter": func(cfg *RunConfig, e *NetemLossEvidence) {
			e.Deterministic.ClientEgress.CounterAfter = e.Deterministic.ClientEgress.CounterBefore - 1
		},
		"packet-count-mismatch": func(cfg *RunConfig, e *NetemLossEvidence) {
			e.Deterministic.ClientEgress.Packets++
		},
		"trace-bits-mismatch": func(cfg *RunConfig, e *NetemLossEvidence) {
			e.Deterministic.ServerEgress.TraceBits = 1 << 13
		},
	} {
		cfg, controlResult, evidence := validDeterministicLossFixture(t)
		mutate(&cfg, evidence)
		if reasons := ValidateNetemLossEvidence(&cfg, controlResult, evidence); len(reasons) == 0 {
			t.Fatalf("%s: tampered evidence accepted", name)
		}
	}
}

func TestValidateDeterministicLossEvidenceMissingEntirely(t *testing.T) {
	cfg, controlResult, _ := validDeterministicLossFixture(t)
	reasons := ValidateNetemLossEvidence(&cfg, controlResult, nil)
	if len(reasons) != 1 || !strings.Contains(reasons[0], "missing for configured deterministic loss") {
		t.Fatalf("reasons = %v", reasons)
	}
}

func TestConfigRejectsMixedRandomAndSeededLoss(t *testing.T) {
	cfg := RunConfig{
		Transport:     "t",
		ServerCommand: CommandConfig{Path: "s"},
		ClientCommand: CommandConfig{Path: "c"},
		ClientProcs:   1, TotalConns: 1,
		Duration: Duration{1_000_000_000}, OutputDir: "out",
		Netem: &NetemRegime{
			ServerEgress: netops.Netem{LossPercent: 1, LossSeed: 42},
			ClientEgress: netops.Netem{LossPercent: 1},
		},
	}
	if _, err := cfg.Prepare(); err == nil || !strings.Contains(err.Error(), "mixes random loss") {
		t.Fatalf("mixed loss accepted (err=%v)", err)
	}
	cfg.Netem.ClientEgress = netops.Netem{LossSeed: 7}
	if _, err := cfg.Prepare(); err == nil || !strings.Contains(err.Error(), "loss_seed requires loss_pct") {
		t.Fatalf("seed without loss accepted (err=%v)", err)
	}
}
