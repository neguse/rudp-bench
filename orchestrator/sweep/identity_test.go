package sweep

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/netops"
	"github.com/neguse/rudp-bench/orchestrator/run"
)

func comparisonFixture() (Config, cellDefinition) {
	scenario := &run.ScenarioSpec{
		Name: "baseline",
		Kind: run.ScenarioEnvironmentBaseline,
		ClientInput: &run.TrafficSpec{
			TrafficID: 1,
			LossTolerant: run.TrafficClassSpec{
				RateHz: 10, PayloadBytes: 64, MinDeliveryRatio: 0.99,
			},
		},
	}
	cfg := Config{
		Regime: "loss1",
		Transports: map[string]TransportSpec{
			"raw": {
				ServerCommand: run.CommandConfig{Path: "missing-server", Args: []string{"--port", "1"}},
				ClientCommand: run.CommandConfig{Path: "missing-client", Args: []string{"--port", "1"}},
				ClientProcs:   2,
			},
		},
		Scenarios:         []run.ScenarioSpec{*scenario},
		Conns:             ConnsRange{Min: 1, Max: 128},
		Seed:              11,
		Warmup:            run.Duration{Duration: time.Second},
		SteadyWarmup:      true,
		Drain:             run.Duration{Duration: time.Second},
		Duration:          run.Duration{Duration: 5 * time.Second},
		StalenessPeriodNS: 10_000_000,
		ServerCPUs:        "7",
		ClientCPUs:        "3-6",
		OutputDir:         "one",
		Netem: &run.NetemRegime{
			ServerEgress: netops.Netem{DelayMS: 10, LossPercent: 1, LossSeed: 100, TraceBits: 1024},
			ClientEgress: netops.Netem{DelayMS: 10, LossPercent: 1, LossSeed: 101, TraceBits: 1024},
		},
	}
	return cfg, cellDefinition{Transport: "raw", Scenario: scenario}
}

func TestComparisonIdentityExcludesReplicateFactors(t *testing.T) {
	cfg, cell := comparisonFixture()
	want := comparisonIdentity(cfg, cell)

	changed := cfg
	changed.Seed++
	changed.OutputDir = "two"
	netem := *cfg.Netem
	netem.ServerEgress.LossSeed++
	netem.ClientEgress.LossSeed++
	changed.Netem = &netem
	if got := comparisonIdentity(changed, cell); got != want {
		t.Fatalf("replicate-only fields changed comparison identity: %s != %s", got, want)
	}
}

func TestComparisonIdentityTracksComparableTreatment(t *testing.T) {
	cfg, cell := comparisonFixture()
	base := comparisonIdentity(cfg, cell)

	tests := map[string]func(Config) Config{
		"timing": func(c Config) Config {
			c.Duration.Duration++
			return c
		},
		"resource": func(c Config) Config {
			c.ClientCPUs = "3-5"
			return c
		},
		"command": func(c Config) Config {
			t := c.Transports["raw"]
			t.ClientCommand.Args = append([]string(nil), t.ClientCommand.Args...)
			t.ClientCommand.Args = append(t.ClientCommand.Args, "--changed")
			c.Transports = map[string]TransportSpec{"raw": t}
			return c
		},
		"netem": func(c Config) Config {
			n := *c.Netem
			n.ServerEgress.DelayMS++
			c.Netem = &n
			return c
		},
		"range": func(c Config) Config {
			c.Conns.Max++
			return c
		},
	}
	for name, change := range tests {
		t.Run(name, func(t *testing.T) {
			if got := comparisonIdentity(change(cfg), cell); got == base {
				t.Fatalf("%s change did not change comparison identity", name)
			}
		})
	}

	copyScenario := *cell.Scenario
	copyTraffic := *copyScenario.ClientInput
	copyTraffic.LossTolerant.RateHz++
	copyScenario.ClientInput = &copyTraffic
	changedCell := cellDefinition{Transport: cell.Transport, Scenario: &copyScenario}
	if got := comparisonIdentity(cfg, changedCell); got == base {
		t.Fatal("scenario/SLO change did not change comparison identity")
	}

	workloadCfg := cfg
	workloadCfg.Scenarios = nil
	workloadCfg.Workloads = []string{"echo", "reliable_echo"}
	if comparisonIdentity(workloadCfg, cellDefinition{Transport: "raw", Workload: "echo"}) ==
		comparisonIdentity(workloadCfg, cellDefinition{Transport: "raw", Workload: "reliable_echo"}) {
		t.Fatal("workload change did not change comparison identity")
	}
}

func TestComparisonIdentityTracksBinaryContent(t *testing.T) {
	cfg, cell := comparisonFixture()
	dir := t.TempDir()
	serverPath := filepath.Join(dir, "server")
	clientPath := filepath.Join(dir, "client")
	if err := os.WriteFile(serverPath, []byte("server-v1"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(clientPath, []byte("client-v1"), 0o755); err != nil {
		t.Fatal(err)
	}
	transport := cfg.Transports["raw"]
	transport.ServerCommand.Path = serverPath
	transport.ClientCommand.Path = clientPath
	cfg.Transports = map[string]TransportSpec{"raw": transport}
	base := comparisonIdentity(cfg, cell)
	if err := os.WriteFile(clientPath, []byte("client-v2"), 0o755); err != nil {
		t.Fatal(err)
	}
	if got := comparisonIdentity(cfg, cell); got == base {
		t.Fatal("binary content change did not change comparison identity")
	}
}

func TestCacheRejectsCrossCampaignRecord(t *testing.T) {
	s := &Sweep{campaignIdentity: "current-campaign"}
	record := PointRecord{CampaignIdentity: "old-campaign", Judgment: Judgment{OK: true}}
	if s.cacheRecordEligible(record) {
		t.Fatal("cross-campaign record unexpectedly eligible for resume cache")
	}
}

func TestLoadResumeAppliesCampaignAndRejudgeCacheSafety(t *testing.T) {
	dir := t.TempDir()
	normal := PointRecord{
		RunIdentity: "normal", CampaignIdentity: "other-campaign",
		Judgment: Judgment{OK: true},
	}
	currentRawInvalid := PointRecord{
		RunIdentity: "current-invalid", CampaignIdentity: "current-campaign",
		Judgment: Judgment{Cause: "invalid: first attempt"},
	}
	currentInvalid := measurementInvalidDisposition(currentRawInvalid)
	otherRawInvalid := PointRecord{
		RunIdentity: "other-invalid", CampaignIdentity: "other-campaign",
		Judgment: Judgment{Cause: "invalid: second attempt"},
	}
	otherInvalid := measurementInvalidDisposition(otherRawInvalid)
	original := PointRecord{
		RunIdentity: "rejudged", CampaignIdentity: "current-campaign",
		Judgment: Judgment{OK: true},
	}
	rejudged := original
	rejudged.Rejudged = true
	rejudged.Judgment = Judgment{Cause: "changed by rejudge"}

	var data []byte
	for _, rec := range []PointRecord{
		normal,
		currentRawInvalid, currentInvalid,
		otherRawInvalid, otherInvalid,
		original, rejudged,
	} {
		line, err := json.Marshal(rec)
		if err != nil {
			t.Fatal(err)
		}
		data = append(data, line...)
		data = append(data, '\n')
	}
	if err := os.WriteFile(filepath.Join(dir, "results.jsonl"), data, 0o644); err != nil {
		t.Fatal(err)
	}

	s := &Sweep{
		cfg: Config{OutputDir: dir}, cache: map[string]PointRecord{},
		campaignIdentity: "current-campaign",
	}
	if err := s.loadResume(); err != nil {
		t.Fatal(err)
	}
	if _, ok := s.cache[normal.RunIdentity]; ok {
		t.Fatal("ordinary cross-campaign measurement was reused")
	}
	if got, ok := s.cache[currentInvalid.RunIdentity]; !ok || !got.MeasurementInvalid {
		t.Fatalf("same-campaign measurement_invalid not preserved: %+v, ok=%v", got, ok)
	}
	if _, ok := s.cache[otherInvalid.RunIdentity]; ok {
		t.Fatal("cross-campaign measurement_invalid fell back to a retry record")
	}
	if _, ok := s.cache[rejudged.RunIdentity]; ok {
		t.Fatal("rejudged record fell back to its original record")
	}
}
