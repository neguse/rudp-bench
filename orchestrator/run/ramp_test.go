package run

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/control"
)

func TestRampConfigDerivesDurationAndLevels(t *testing.T) {
	scenario := authoritativeFixture()
	cfg := RunConfig{
		Transport:     "fake",
		Scenario:      &scenario,
		ServerCommand: CommandConfig{Path: "server"},
		ClientCommand: CommandConfig{Path: "client"},
		ClientProcs:   1,
		TotalConns:    10,
		Ramp: &RampConfig{
			StartConns: 1,
			StepConns:  3,
			Guard:      Duration{Duration: 200 * time.Millisecond},
			Sample:     Duration{Duration: time.Second},
			Drain:      Duration{Duration: 100 * time.Millisecond},
		},
		Duration:  Duration{Duration: 99 * time.Second},
		OutputDir: "out",
	}
	prepared, err := cfg.Prepare()
	if err != nil {
		t.Fatal(err)
	}
	// 1,4,7,10 = four phases, each 1.3s. Explicit duration is intentionally
	// replaced because endpoint phase timing is the duration contract.
	if prepared.Duration.Duration != 5200*time.Millisecond {
		t.Fatalf("duration=%s, want 5.2s", prepared.Duration.Duration)
	}
	levels, err := prepared.Ramp.levels(prepared.TotalConns)
	if err != nil || levels != 4 {
		t.Fatalf("levels=%d err=%v, want 4", levels, err)
	}
	want := []int{1, 4, 7, 10}
	for i := range want {
		if got := prepared.Ramp.activeConns(i, prepared.TotalConns); got != want[i] {
			t.Fatalf("active[%d]=%d, want %d", i, got, want[i])
		}
	}
}

func TestRampConfigValidation(t *testing.T) {
	scenario := authoritativeFixture()
	base := RunConfig{
		Transport:     "fake",
		Scenario:      &scenario,
		ServerCommand: CommandConfig{Path: "server"},
		ClientCommand: CommandConfig{Path: "client"},
		ClientProcs:   1,
		TotalConns:    8,
		Ramp: &RampConfig{
			StartConns: 1,
			StepConns:  1,
			Sample:     Duration{Duration: time.Second},
			Drain:      Duration{Duration: 100 * time.Millisecond},
		},
		OutputDir: "out",
	}
	for name, mutate := range map[string]func(*RunConfig){
		"multiple clients": func(cfg *RunConfig) { cfg.ClientProcs = 2 },
		"no scenario":      func(cfg *RunConfig) { cfg.Scenario = nil },
		"zero start":       func(cfg *RunConfig) { cfg.Ramp.StartConns = 0 },
		"start over max":   func(cfg *RunConfig) { cfg.Ramp.StartConns = 9 },
		"zero step":        func(cfg *RunConfig) { cfg.Ramp.StepConns = 0 },
		"zero sample":      func(cfg *RunConfig) { cfg.Ramp.Sample.Duration = 0 },
		"short drain":      func(cfg *RunConfig) { cfg.Ramp.Drain.Duration = 99 * time.Millisecond },
		"steady":           func(cfg *RunConfig) { cfg.SteadyWarmup = true },
	} {
		t.Run(name, func(t *testing.T) {
			cfg := base
			ramp := *base.Ramp
			cfg.Ramp = &ramp
			mutate(&cfg)
			if _, err := cfg.Prepare(); err == nil {
				t.Fatal("invalid ramp config was accepted")
			}
		})
	}
}

func TestScoreRampSnapshotsFirstViolation(t *testing.T) {
	cfg, serverBase, clientBase := rampSnapshotFixture(t, 5)
	writePassingAuthoritativeRampPhase(t, cfg, serverBase, clientBase, 0, 1, false)
	writePassingAuthoritativeRampPhase(t, cfg, serverBase, clientBase, 1, 3, true)

	result, err := scoreRampSnapshots(cfg, serverBase, []string{clientBase})
	if err != nil {
		t.Fatal(err)
	}
	if result.Censored || result.ScoreConns != 3 {
		t.Fatalf("ramp result=%+v, want first failure at 3", result)
	}
	if len(result.Timeline) != 2 || !result.Timeline[0].Evaluation.OK ||
		result.Timeline[1].Evaluation.OK {
		t.Fatalf("timeline evaluations=%+v", result.Timeline)
	}
	if !strings.Contains(result.Cause, "delivery") {
		t.Fatalf("cause=%q, want delivery failure", result.Cause)
	}
}

func TestScoreRampSnapshotsCensorRequiresEveryPhase(t *testing.T) {
	cfg, serverBase, clientBase := rampSnapshotFixture(t, 5)
	writePassingAuthoritativeRampPhase(t, cfg, serverBase, clientBase, 0, 1, false)
	writePassingAuthoritativeRampPhase(t, cfg, serverBase, clientBase, 1, 3, false)

	if _, err := scoreRampSnapshots(cfg, serverBase, []string{clientBase}); err == nil ||
		!errors.Is(err, errRampSnapshotPending) {
		t.Fatalf("missing censor tail error=%v, want pending snapshot", err)
	}
}

func TestRampPhaseCoverageShortageIsBreakButExcessIsInvalid(t *testing.T) {
	t.Run("slot shortage", func(t *testing.T) {
		cfg, serverBase, clientBase := rampSnapshotFixture(t, 1)
		writePassingAuthoritativeRampPhase(t, cfg, serverBase, clientBase, 0, 1, false)
		rewriteRampPhase(t, serverBase, clientBase, 0, 1, func(role string, file *metricsFile) {
			input := cfg.Scenario.ClientInput
			metric := findFixtureTraffic(t, file, input.TrafficID, DirectionClientToServer, ClassLossTolerant)
			if role == "client" {
				metric.Slots--
				metric.Submitted--
			} else {
				metric.DeliveredUnique--
			}
		})
		result, err := scoreRampSnapshots(cfg, serverBase, []string{clientBase})
		if err != nil {
			t.Fatal(err)
		}
		if result.ScoreConns != 1 || !strings.Contains(result.Cause, "slots=") {
			t.Fatalf("shortage result=%+v", result)
		}
	})

	t.Run("attempted shortage", func(t *testing.T) {
		cfg, serverBase, clientBase := rampSnapshotFixture(t, 1)
		writePassingAuthoritativeRampPhase(t, cfg, serverBase, clientBase, 0, 1, false)
		rewriteRampPhase(t, serverBase, clientBase, 0, 1, func(role string, file *metricsFile) {
			if role != "client" {
				return
			}
			input := cfg.Scenario.ClientInput
			metric := findFixtureTraffic(t, file, input.TrafficID, DirectionClientToServer, ClassLossTolerant)
			metric.Submitted--
		})
		result, err := scoreRampSnapshots(cfg, serverBase, []string{clientBase})
		if err != nil {
			t.Fatal(err)
		}
		if result.ScoreConns != 1 || !strings.Contains(result.Cause, "attempted=") {
			t.Fatalf("attempted shortage result=%+v", result)
		}
	})

	t.Run("staleness shortage", func(t *testing.T) {
		cfg, serverBase, clientBase := rampSnapshotFixture(t, 1)
		writePassingAuthoritativeRampPhase(t, cfg, serverBase, clientBase, 0, 1, false)
		rewriteRampPhase(t, serverBase, clientBase, 0, 1, func(role string, file *metricsFile) {
			if role != "server" {
				return
			}
			input := cfg.Scenario.ClientInput
			metric := findFixtureTraffic(t, file, input.TrafficID, DirectionClientToServer, ClassLossTolerant)
			metric.StalenessNS = emptyV2Histogram()
		})
		result, err := scoreRampSnapshots(cfg, serverBase, []string{clientBase})
		if err != nil {
			t.Fatal(err)
		}
		if result.ScoreConns != 1 || !strings.Contains(result.Cause, "staleness samples=") {
			t.Fatalf("staleness shortage result=%+v", result)
		}
	})

	t.Run("missing zero-slot traffic", func(t *testing.T) {
		cfg, serverBase, clientBase := rampSnapshotFixture(t, 1)
		writePassingAuthoritativeRampPhase(t, cfg, serverBase, clientBase, 0, 1, false)
		rewriteRampPhase(t, serverBase, clientBase, 0, 1, func(_ string, file *metricsFile) {
			file.Traffic = removeTrafficMetric(file.Traffic, DirectionClientToServer, ClassMustDeliver)
		})
		result, err := scoreRampSnapshots(cfg, serverBase, []string{clientBase})
		if err != nil {
			t.Fatal(err)
		}
		if result.ScoreConns != 1 || !strings.Contains(result.Cause, "metrics missing") {
			t.Fatalf("missing traffic result=%+v", result)
		}
	})

	t.Run("slot excess", func(t *testing.T) {
		cfg, serverBase, clientBase := rampSnapshotFixture(t, 1)
		writePassingAuthoritativeRampPhase(t, cfg, serverBase, clientBase, 0, 1, false)
		rewriteRampPhase(t, serverBase, clientBase, 0, 1, func(role string, file *metricsFile) {
			input := cfg.Scenario.ClientInput
			metric := findFixtureTraffic(t, file, input.TrafficID, DirectionClientToServer, ClassLossTolerant)
			_, maxSlots, err := expectedSlotRange(input.LossTolerant.RateHz, cfg.Ramp.Sample.Duration, 1)
			if err != nil {
				t.Fatal(err)
			}
			if role == "client" {
				metric.Slots = maxSlots + 1
				metric.Submitted = maxSlots + 1
			} else {
				metric.DeliveredUnique = maxSlots + 1
			}
		})
		if _, err := scoreRampSnapshots(cfg, serverBase, []string{clientBase}); err == nil ||
			!strings.Contains(err.Error(), "exceeds expected maximum") {
			t.Fatalf("slot excess error=%v", err)
		}
	})

	t.Run("flow mismatch", func(t *testing.T) {
		cfg, serverBase, clientBase := rampSnapshotFixture(t, 1)
		writePassingAuthoritativeRampPhase(t, cfg, serverBase, clientBase, 0, 1, false)
		rewriteRampPhase(t, serverBase, clientBase, 0, 1, func(role string, file *metricsFile) {
			if role != "server" {
				return
			}
			input := cfg.Scenario.ClientInput
			metric := findFixtureTraffic(t, file, input.TrafficID, DirectionClientToServer, ClassLossTolerant)
			metric.ExpectedFlows = 0
			metric.ObservedFlows = 0
			metric.NeverReceivedFlows = 0
		})
		if _, err := scoreRampSnapshots(cfg, serverBase, []string{clientBase}); err == nil ||
			!strings.Contains(err.Error(), "expected_flows") {
			t.Fatalf("flow mismatch error=%v", err)
		}
	})
}

func TestWatchRampSnapshotsRetriesPartialPairBeforeStopping(t *testing.T) {
	cfg, serverBase, clientBase := rampSnapshotFixture(t, 3)
	writePassingAuthoritativeRampPhase(t, cfg, serverBase, clientBase, 0, 1, true)
	serverPath := rampSnapshotPath(serverBase, 0, 1)
	completeServer, err := os.ReadFile(serverPath)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(serverPath, []byte(`{"version":2`), 0o644); err != nil {
		t.Fatal(err)
	}

	stopPath := filepath.Join(filepath.Dir(serverBase), "ramp.stop")
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	done := make(chan error, 1)
	go func() {
		done <- watchRampSnapshots(ctx, cfg, serverBase, []string{clientBase}, stopPath)
	}()

	time.Sleep(40 * time.Millisecond)
	if _, err := os.Stat(stopPath); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("stop marker published from partial pair: %v", err)
	}
	if err := os.WriteFile(serverPath, completeServer, 0o644); err != nil {
		t.Fatal(err)
	}
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-ctx.Done():
		t.Fatal("watcher did not publish stop marker for completed failing pair")
	}
	if _, err := os.Stat(stopPath); err != nil {
		t.Fatalf("stop marker: %v", err)
	}
}

func TestScoreRampSnapshotsCensored(t *testing.T) {
	cfg, serverBase, clientBase := rampSnapshotFixture(t, 3)
	writePassingAuthoritativeRampPhase(t, cfg, serverBase, clientBase, 0, 1, false)
	writePassingAuthoritativeRampPhase(t, cfg, serverBase, clientBase, 1, 3, false)

	result, err := scoreRampSnapshots(cfg, serverBase, []string{clientBase})
	if err != nil {
		t.Fatal(err)
	}
	if !result.Censored || result.ScoreConns != 0 ||
		!strings.Contains(result.Cause, "through 3 connections") {
		t.Fatalf("censored result=%+v", result)
	}
}

func TestScoreRampSnapshotsRoomUsesClientMetricsAtActiveFanout(t *testing.T) {
	dir := t.TempDir()
	scenario := ScenarioSpec{
		Name: "room-ramp-test",
		Kind: ScenarioRoomRelay,
		RoomPublish: &TrafficSpec{
			TrafficID: 3,
			LossTolerant: TrafficClassSpec{RateHz: 20, PayloadBytes: 128,
				MinDeliveryRatio: 0.9, StalenessP99NS: 100_000_000},
			MustDeliver: TrafficClassSpec{RateHz: 5, PayloadBytes: 64, DeadlineNS: 200_000_000,
				MinDeadlineHitRatio: 0.9, MinEventualDeliveryRatio: 0.99},
		},
	}
	cfg := RunConfig{
		Scenario: &scenario, ClientProcs: 1, TotalConns: 3,
		Ramp: &RampConfig{StartConns: 1, StepConns: 2, Sample: Duration{Duration: time.Second},
			Drain: Duration{Duration: 200 * time.Millisecond}},
		AttemptedThreshold: defaultAttemptedThreshold,
		StalenessPeriodNS:  defaultStalenessPeriodNS,
	}
	serverBase := filepath.Join(dir, "server.json")
	clientBase := filepath.Join(dir, "client-0.json")
	writeRoomRampPhase(t, cfg, serverBase, clientBase, 0, 1, false)
	writeRoomRampPhase(t, cfg, serverBase, clientBase, 1, 3, true)

	result, err := scoreRampSnapshots(cfg, serverBase, []string{clientBase})
	if err != nil {
		t.Fatal(err)
	}
	if result.ScoreConns != 3 || result.Censored {
		t.Fatalf("room ramp result=%+v", result)
	}
	metricCause := result.Timeline[1].Evaluation.Cause
	if !strings.Contains(metricCause, "delivery") {
		t.Fatalf("room failure cause=%q", metricCause)
	}
}

func TestScoreRampSnapshotsRejectsActiveMismatchAndBadJSON(t *testing.T) {
	t.Run("active mismatch", func(t *testing.T) {
		cfg, serverBase, clientBase := rampSnapshotFixture(t, 1)
		writePassingAuthoritativeRampPhase(t, cfg, serverBase, clientBase, 0, 1, false)
		good := rampSnapshotPath(clientBase, 0, 1)
		bad := rampSnapshotPath(clientBase, 0, 2)
		if err := os.Rename(good, bad); err != nil {
			t.Fatal(err)
		}
		if _, err := scoreRampSnapshots(cfg, serverBase, []string{clientBase}); err == nil ||
			!strings.Contains(err.Error(), "active connections") {
			t.Fatalf("active mismatch error=%v", err)
		}
	})

	t.Run("malformed metrics", func(t *testing.T) {
		cfg, serverBase, clientBase := rampSnapshotFixture(t, 1)
		writePassingAuthoritativeRampPhase(t, cfg, serverBase, clientBase, 0, 1, false)
		if err := os.WriteFile(rampSnapshotPath(clientBase, 0, 1), []byte(`{"version":`), 0o644); err != nil {
			t.Fatal(err)
		}
		if _, err := scoreRampSnapshots(cfg, serverBase, []string{clientBase}); err == nil ||
			!strings.Contains(err.Error(), "decode metrics") {
			t.Fatalf("malformed metrics error=%v", err)
		}
	})
}

func TestRampGateDoesNotRequireFixedFinalMetricsOrDoneProgress(t *testing.T) {
	scenario := authoritativeFixture()
	cfg := RunConfig{Scenario: &scenario, Ramp: &RampConfig{StartConns: 1, StepConns: 1,
		Sample: Duration{Duration: time.Second}}}
	controlResult := controlResultWithMalformedDoneStats()
	gate := EvaluateGate(GateInput{
		Config:  &cfg,
		Control: &controlResult,
	})
	if gate.Verdict != VerdictValid {
		t.Fatalf("ramp gate verdict=%s reasons=%v", gate.Verdict, gate.Reasons)
	}
}

func TestClassifyRampOutcome(t *testing.T) {
	result := &Result{
		Verdict: VerdictValid,
		Config:  RunConfig{Ramp: &RampConfig{}},
		Ramp:    &RampResult{ScoreConns: 17, Cause: "delivery below SLO"},
	}
	if outcome, _ := classifyRunOutcome(result, GateInput{}); outcome != OutcomeFail {
		t.Fatalf("break outcome=%s, want FAIL", outcome)
	}
	result.Ramp = &RampResult{Censored: true, Cause: "no SLO violation through 32 connections"}
	if outcome, _ := classifyRunOutcome(result, GateInput{}); outcome != OutcomeCensored {
		t.Fatalf("censored outcome=%s, want CENSORED", outcome)
	}
	result.Verdict = VerdictInvalid
	result.InvalidReasons = []string{"ramp metrics contract: malformed JSON"}
	if outcome, _ := classifyRunOutcome(result, GateInput{SUTFailureReasons: []string{"client exited"}}); outcome != OutcomeInvalid {
		t.Fatalf("invalid ramp outcome=%s, want INVALID", outcome)
	}
}

func controlResultWithMalformedDoneStats() control.Result {
	return control.Result{Participants: []control.Participant{{
		Hello: control.HelloMessage{Role: "client", ProcIndex: 0},
		Done:  control.DoneMessage{Stats: []byte(`not-json`)},
	}}}
}

func rampSnapshotFixture(t *testing.T, total int) (RunConfig, string, string) {
	t.Helper()
	dir := t.TempDir()
	scenario := authoritativeFixture()
	cfg := RunConfig{
		Scenario:           &scenario,
		ClientProcs:        1,
		TotalConns:         total,
		AttemptedThreshold: defaultAttemptedThreshold,
		StalenessPeriodNS:  defaultStalenessPeriodNS,
		Ramp: &RampConfig{
			StartConns: 1,
			StepConns:  2,
			Sample:     Duration{Duration: time.Second},
			Drain:      Duration{Duration: 100 * time.Millisecond},
		},
	}
	return cfg, filepath.Join(dir, "server.json"), filepath.Join(dir, "client-0.json")
}

func rampSnapshotPath(base string, index, active int) string {
	return fmt.Sprintf("%s.ramp-%06d-c%06d.json", base, index, active)
}

func rewriteRampPhase(
	t *testing.T,
	serverBase, clientBase string,
	index, active int,
	mutate func(role string, file *metricsFile),
) {
	t.Helper()
	for _, endpoint := range []struct {
		role string
		base string
	}{
		{role: "server", base: serverBase},
		{role: "client", base: clientBase},
	} {
		path := rampSnapshotPath(endpoint.base, index, active)
		file, err := readMetricsFile(path)
		if err != nil {
			t.Fatal(err)
		}
		mutate(endpoint.role, &file)
		writeScenarioMetricsFile(t, path, file)
	}
}

func writePassingAuthoritativeRampPhase(
	t *testing.T,
	cfg RunConfig,
	serverBase, clientBase string,
	index, active int,
	failInputDelivery bool,
) {
	t.Helper()
	server := authoritativeTrafficFixture(*cfg.Scenario, "server", active, active)
	client := authoritativeTrafficFixture(*cfg.Scenario, "client", active, active)

	for i := range client.Traffic {
		sender := &client.Traffic[i]
		if sender.Direction != DirectionClientToServer {
			continue
		}
		receiver := findFixtureTraffic(t, &server, sender.TrafficID, sender.Direction, sender.Class)
		receiver.DeliveredUnique = sender.Slots
		if sender.Class == ClassMustDeliver {
			receiver.DeadlineHit = sender.Slots
		}
	}
	for i := range server.Traffic {
		sender := &server.Traffic[i]
		if sender.Direction != DirectionServerToClient {
			continue
		}
		receiver := findFixtureTraffic(t, &client, sender.TrafficID, sender.Direction, sender.Class)
		receiver.DeliveredUnique = sender.Slots
		if sender.Class == ClassMustDeliver {
			receiver.DeadlineHit = sender.Slots
		}
	}
	if failInputDelivery {
		input := cfg.Scenario.ClientInput
		receiver := findFixtureTraffic(t, &server, input.TrafficID, DirectionClientToServer, ClassLossTolerant)
		receiver.DeliveredUnique = 0
	}

	writeScenarioMetricsFile(t, rampSnapshotPath(serverBase, index, active), server)
	writeScenarioMetricsFile(t, rampSnapshotPath(clientBase, index, active), client)
}

func writeRoomRampPhase(
	t *testing.T,
	cfg RunConfig,
	serverBase, clientBase string,
	index, active int,
	failDelivery bool,
) {
	t.Helper()
	client := authoritativeTrafficFixture(*cfg.Scenario, "client", active, active)
	for i := range client.Traffic {
		traffic := &client.Traffic[i]
		traffic.SlotsBroadcast = traffic.Slots
		traffic.DeliveredUnique = traffic.Slots * uint64(active)
		if traffic.Class == ClassMustDeliver {
			traffic.DeadlineHit = traffic.DeliveredUnique
		}
	}
	if failDelivery {
		metric := findFixtureTraffic(t, &client, cfg.Scenario.RoomPublish.TrafficID,
			DirectionRoomRelay, ClassLossTolerant)
		metric.DeliveredUnique = 0
	}
	server := metricsFile{
		Version: 2,
		Histogram: HistogramLayout{
			Scheme: "log2x16", Subbins: 16, MinNS: 1_000, MaxNS: 100_000_000_000,
		},
		Traffic: []trafficMetric{},
	}
	writeScenarioMetricsFile(t, rampSnapshotPath(serverBase, index, active), server)
	writeScenarioMetricsFile(t, rampSnapshotPath(clientBase, index, active), client)
}

func findFixtureTraffic(t *testing.T, file *metricsFile, id uint8, direction, class string) *trafficMetric {
	t.Helper()
	for i := range file.Traffic {
		traffic := &file.Traffic[i]
		if traffic.TrafficID == id && traffic.Direction == direction && traffic.Class == class {
			return traffic
		}
	}
	t.Fatalf("fixture traffic missing id=%d direction=%s class=%s", id, direction, class)
	return nil
}
