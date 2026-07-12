package run

import (
	"fmt"
	"sort"
	"strings"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/netops"
	"github.com/neguse/rudp-bench/orchestrator/rig"
)

// Reference preset は ADR-0004 で凍結した比較カタログ条件。config が preset を
// 指名した場合、preset が固定する値は一切上書きできない(ADR-0002)。
// 変更は新しい -vN を追加して行い、既存 preset の値は変えない。

type ReferencePreset struct {
	Name              string
	Scenario          ScenarioSpec
	ClientEgress      netops.Netem
	ServerEgress      netops.Netem
	LinkMTUBytes      int
	DisableOffloads   bool
	Warmup            time.Duration
	Duration          time.Duration
	Drain             time.Duration
	StalenessPeriodNS uint64
	// SUT server の resource budget(ADR-0004: 2 vCPU 相当)。
	// server_cpus がちょうどこの個数であることを要求する
	ServerVCPUs int
}

const (
	refLTStalenessP99NS   = 300_000_000
	refLTMinDeliveryRatio = 0.95
	refMDDeadlineNS       = 200_000_000
	refMDMinDeadlineHit   = 0.95
)

func refLT(rateHz float64, payloadBytes int) TrafficClassSpec {
	return TrafficClassSpec{
		RateHz: rateHz, PayloadBytes: payloadBytes,
		StalenessP99NS: refLTStalenessP99NS, MinDeliveryRatio: refLTMinDeliveryRatio,
	}
}

func refMD() TrafficClassSpec {
	return TrafficClassSpec{
		RateHz: 10, PayloadBytes: 64,
		DeadlineNS: refMDDeadlineNS, MinDeadlineHitRatio: refMDMinDeadlineHit,
		MinEventualDeliveryRatio: 1,
	}
}

var refRegimes = map[string]netops.Netem{
	"lan": {DelayMS: 1},
	"wan": {DelayMS: 25, LossPercent: 1},
}

func referencePresets() map[string]ReferencePreset {
	presets := map[string]ReferencePreset{}
	add := func(name string, scenario ScenarioSpec, egress netops.Netem) {
		scenario.Name = name
		presets[name] = ReferencePreset{
			Name: name, Scenario: scenario,
			ClientEgress: egress, ServerEgress: egress,
			LinkMTUBytes: 1500, DisableOffloads: true,
			Warmup: 25 * time.Second, Duration: 60 * time.Second, Drain: 5 * time.Second,
			StalenessPeriodNS: defaultStalenessPeriodNS,
			ServerVCPUs:       2,
		}
	}
	for regime, egress := range refRegimes {
		for _, statePayload := range []int{1000, 4000} {
			add(fmt.Sprintf("ref-auth-%s-s%d-v1", regime, statePayload), ScenarioSpec{
				Kind: ScenarioAuthoritativeState,
				ClientInput: &TrafficSpec{
					TrafficID:    TrafficIDClientInput,
					LossTolerant: refLT(30, 64),
					MustDeliver:  refMD(),
				},
				ServerState: &TrafficSpec{
					TrafficID:    TrafficIDServerState,
					LossTolerant: refLT(20, statePayload),
					MustDeliver:  refMD(),
				},
			}, egress)
		}
		add(fmt.Sprintf("ref-room-%s-v1", regime), ScenarioSpec{
			Kind: ScenarioRoomRelay,
			RoomPublish: &TrafficSpec{
				TrafficID:    3,
				LossTolerant: refLT(20, 128),
				MustDeliver:  refMD(),
			},
		}, egress)
	}
	return presets
}

func ReferencePresetNames() []string {
	names := make([]string, 0)
	for name := range referencePresets() {
		names = append(names, name)
	}
	sort.Strings(names)
	return names
}

func (cfg *RunConfig) applyPreset() error {
	if cfg.Preset == "" {
		return nil
	}
	preset, ok := referencePresets()[cfg.Preset]
	if !ok {
		return fmt.Errorf("unknown reference preset %q (known: %s)",
			cfg.Preset, strings.Join(ReferencePresetNames(), ", "))
	}
	for field, overridden := range map[string]bool{
		"scenario":            cfg.Scenario != nil,
		"workload":            cfg.Workload != "",
		"netem":               cfg.Netem != nil,
		"warmup":              cfg.Warmup.Duration != 0,
		"duration":            cfg.Duration.Duration != 0,
		"drain":               cfg.Drain.Duration != 0,
		"staleness_period_ns": cfg.StalenessPeriodNS != 0,
		"steady_warmup":       cfg.SteadyWarmup,
		"ramp":                cfg.Ramp != nil,
	} {
		if overridden {
			return fmt.Errorf("preset %q pins %s; remove it from the config", cfg.Preset, field)
		}
	}
	serverCPUs, err := rig.ParseCPUSet(cfg.ServerCPUs)
	if err != nil || len(serverCPUs) != preset.ServerVCPUs {
		return fmt.Errorf("preset %q requires server_cpus with exactly %d CPUs (resource budget), got %q",
			cfg.Preset, preset.ServerVCPUs, cfg.ServerCPUs)
	}
	scenario := preset.Scenario
	cfg.Scenario = &scenario
	cfg.Netem = &NetemRegime{
		LinkMTUBytes:    preset.LinkMTUBytes,
		DisableOffloads: preset.DisableOffloads,
		ServerEgress:    preset.ServerEgress,
		ClientEgress:    preset.ClientEgress,
	}
	cfg.Warmup = Duration{preset.Warmup}
	cfg.SteadyWarmup = true
	cfg.Duration = Duration{preset.Duration}
	cfg.Drain = Duration{preset.Drain}
	cfg.StalenessPeriodNS = preset.StalenessPeriodNS
	return nil
}
