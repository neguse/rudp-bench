package run

import (
	"strings"
	"testing"
	"time"
)

func referencePresetConfig(preset string) RunConfig {
	return RunConfig{
		Transport:     "enet",
		Preset:        preset,
		ServerCommand: CommandConfig{Path: "bin/server"},
		ClientCommand: CommandConfig{Path: "bin/client"},
		ClientProcs:   1,
		TotalConns:    3,
		ServerCPUs:    "7,15",
		ClientCPUs:    "3,4",
		OutputDir:     "out",
	}
}

func TestReferencePresetExpandsFrozenValues(t *testing.T) {
	cfg, err := referencePresetConfig("ref-auth-wan-s1000-v1").Prepare()
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Warmup.Duration != 25*time.Second || cfg.Duration.Duration != 60*time.Second ||
		cfg.Drain.Duration != 5*time.Second || !cfg.SteadyWarmup {
		t.Fatalf("frozen durations not applied: %+v", cfg)
	}
	if cfg.StalenessPeriodNS != 10_000_000 {
		t.Fatalf("staleness period = %d", cfg.StalenessPeriodNS)
	}
	s := cfg.Scenario
	if s == nil || s.Name != "ref-auth-wan-s1000-v1" || s.Kind != ScenarioAuthoritativeState {
		t.Fatalf("scenario = %+v", s)
	}
	if s.ClientInput.LossTolerant.RateHz != 30 || s.ClientInput.LossTolerant.PayloadBytes != 64 {
		t.Fatalf("client_input = %+v", s.ClientInput)
	}
	if s.ServerState.LossTolerant.RateHz != 20 || s.ServerState.LossTolerant.PayloadBytes != 1000 {
		t.Fatalf("server_state = %+v", s.ServerState)
	}
	if s.ServerState.LossTolerant.StalenessP99NS != 300_000_000 ||
		s.ServerState.LossTolerant.MinDeliveryRatio != 0.95 {
		t.Fatalf("LT SLO = %+v", s.ServerState.LossTolerant)
	}
	if s.ServerState.MustDeliver.DeadlineNS != 200_000_000 ||
		s.ServerState.MustDeliver.MinDeadlineHitRatio != 0.95 ||
		s.ServerState.MustDeliver.MinEventualDeliveryRatio != 1 {
		t.Fatalf("MD SLO = %+v", s.ServerState.MustDeliver)
	}
	n := cfg.Netem
	if n == nil || n.ClientEgress.DelayMS != 25 || n.ClientEgress.LossPercent != 1 ||
		n.ServerEgress.DelayMS != 25 || n.ServerEgress.LossPercent != 1 {
		t.Fatalf("netem = %+v", n)
	}
	if n.LinkMTUBytes != 1500 || !n.DisableOffloads {
		t.Fatalf("link settings = %+v", n)
	}
}

func TestReferencePresetVariants(t *testing.T) {
	cfg, err := referencePresetConfig("ref-auth-lan-s4000-v1").Prepare()
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Scenario.ServerState.LossTolerant.PayloadBytes != 4000 {
		t.Fatalf("s4000 payload = %d", cfg.Scenario.ServerState.LossTolerant.PayloadBytes)
	}
	if cfg.Netem.ClientEgress.LossPercent != 0 || cfg.Netem.ClientEgress.DelayMS != 1 {
		t.Fatalf("lan netem = %+v", cfg.Netem.ClientEgress)
	}
	room, err := referencePresetConfig("ref-room-wan-v1").Prepare()
	if err != nil {
		t.Fatal(err)
	}
	if room.Scenario.Kind != ScenarioRoomRelay ||
		room.Scenario.RoomPublish.LossTolerant.RateHz != 20 ||
		room.Scenario.RoomPublish.LossTolerant.PayloadBytes != 128 {
		t.Fatalf("room scenario = %+v", room.Scenario)
	}
}

func TestReferencePresetsAreCompleteAndValid(t *testing.T) {
	names := ReferencePresetNames()
	if len(names) != 6 {
		t.Fatalf("presets = %v, want 6", names)
	}
	for _, name := range names {
		cfg, err := referencePresetConfig(name).Prepare()
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		if !cfg.Scenario.HasCompletePrimarySLOs() {
			t.Fatalf("%s: missing primary SLOs: %v", name, cfg.Scenario.MissingPrimarySLOs())
		}
	}
}

func TestReferencePresetRejectsOverrides(t *testing.T) {
	for field, mutate := range map[string]func(*RunConfig){
		"scenario": func(c *RunConfig) {
			c.Scenario = &ScenarioSpec{Name: "x", Kind: ScenarioRoomRelay}
		},
		"workload":  func(c *RunConfig) { c.Workload = "r20p128" },
		"netem":     func(c *RunConfig) { c.Netem = &NetemRegime{} },
		"warmup":    func(c *RunConfig) { c.Warmup = Duration{time.Second} },
		"duration":  func(c *RunConfig) { c.Duration = Duration{time.Second} },
		"drain":     func(c *RunConfig) { c.Drain = Duration{time.Second} },
		"staleness": func(c *RunConfig) { c.StalenessPeriodNS = 1 },
	} {
		cfg := referencePresetConfig("ref-auth-wan-s1000-v1")
		mutate(&cfg)
		if _, err := cfg.Prepare(); err == nil || !strings.Contains(err.Error(), "preset") {
			t.Fatalf("%s override accepted (err=%v)", field, err)
		}
	}
}

func TestReferencePresetRequiresServerBudget(t *testing.T) {
	cfg := referencePresetConfig("ref-auth-wan-s1000-v1")
	cfg.ServerCPUs = "7"
	if _, err := cfg.Prepare(); err == nil || !strings.Contains(err.Error(), "server_cpus") {
		t.Fatalf("1-CPU budget accepted (err=%v)", err)
	}
	cfg.ServerCPUs = ""
	if _, err := cfg.Prepare(); err == nil || !strings.Contains(err.Error(), "server_cpus") {
		t.Fatalf("missing budget accepted (err=%v)", err)
	}
}

func TestReferencePresetUnknownName(t *testing.T) {
	if _, err := referencePresetConfig("ref-nope-v1").Prepare(); err == nil {
		t.Fatal("unknown preset accepted")
	}
}

func TestConfirmatoryV1FrozenValues(t *testing.T) {
	p := ConfirmatoryV1()
	if p.BlockN != 3 || p.FlapBlockN != 5 || p.MaxBlocks != 5 || p.StoppingSpreadMax != 0.05 {
		t.Fatalf("stopping rule = %+v", p)
	}
	if p.DriftMaxDeliveryDelta != 0.010 || p.DriftMaxStalenessP99Ratio != 1.10 {
		t.Fatalf("drift tolerances = %+v", p)
	}
}

func TestPresetHashCoversAllPresets(t *testing.T) {
	seen := map[string]string{}
	for _, name := range ReferencePresetNames() {
		hash, err := PresetHash(name)
		if err != nil {
			t.Fatalf("%s: %v", name, err)
		}
		if hash == "" {
			t.Fatalf("%s: empty hash", name)
		}
		if prev, ok := seen[hash]; ok {
			t.Fatalf("hash collision: %s and %s", prev, name)
		}
		seen[hash] = name
		again, err := PresetHash(name)
		if err != nil || again != hash {
			t.Fatalf("%s: hash not stable: %s vs %s (%v)", name, hash, again, err)
		}
	}
}

func TestPresetHashUnknownName(t *testing.T) {
	if _, err := PresetHash("ref-nope-v1"); err == nil {
		t.Fatal("unknown preset hashed")
	}
}

func TestLookupReferencePreset(t *testing.T) {
	preset, ok := LookupReferencePreset("ref-room-wan-v1")
	if !ok || preset.Scenario.Kind != ScenarioRoomRelay || preset.ServerVCPUs != 2 {
		t.Fatalf("lookup = %+v ok=%v", preset, ok)
	}
	netem := preset.NetemRegime()
	if netem == nil || netem.LinkMTUBytes != 1500 || !netem.DisableOffloads ||
		netem.ClientEgress.DelayMS != 25 || netem.ClientEgress.LossPercent != 1 {
		t.Fatalf("netem = %+v", netem)
	}
	if _, ok := LookupReferencePreset("ref-nope-v1"); ok {
		t.Fatal("unknown preset found")
	}
}
