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

// ConfirmatoryProtocol は pilot から凍結した confirmatory の判定規則
// (ADR-0004 §3 の手順で導出、2026-07-23 owner 承認)。変更は ADR-0004 を
// supersede する新 ADR を要する。
type ConfirmatoryProtocol struct {
	// block 反復数。境界 flap が IQR を超える cell のみ FlapBlockN へ拡張
	BlockN     int `json:"block_n"`
	FlapBlockN int `json:"flap_block_n"`
	// 停止規則: 直近連続 BlockN block の全幅/median <= StoppingSpreadMax で
	// 確定。MaxBlocks で未達なら INCONCLUSIVE
	MaxBlocks         int     `json:"max_blocks"`
	StoppingSpreadMax float64 `json:"stopping_spread_max"`
	// block 前後 baseline の drift 許容幅(量ごとの単位):
	// delivery ratio は前後の絶対差、staleness p99 は前後比の上限
	DriftMaxDeliveryDelta     float64 `json:"drift_max_delivery_delta"`
	DriftMaxStalenessP99Ratio float64 `json:"drift_max_staleness_p99_ratio"`
}

func ConfirmatoryV1() ConfirmatoryProtocol {
	return ConfirmatoryProtocol{
		BlockN: 3, FlapBlockN: 5, MaxBlocks: 5, StoppingSpreadMax: 0.05,
		DriftMaxDeliveryDelta: 0.010, DriftMaxStalenessP99Ratio: 1.10,
	}
}

const presetHashVersion = 1

// PresetHash は preset の正規化パラメータと confirmatory protocol 凍結値の
// 指紋。結果に埋め込み「同じ凍結条件で測った」ことを機械的に保証する
// (ADR-0004 Consequences)。
func PresetHash(name string) (string, error) {
	preset, ok := referencePresets()[name]
	if !ok {
		return "", fmt.Errorf("unknown reference preset %q (known: %s)",
			name, strings.Join(ReferencePresetNames(), ", "))
	}
	return HashValue(struct {
		Version  int                  `json:"version"`
		Preset   ReferencePreset      `json:"preset"`
		Protocol ConfirmatoryProtocol `json:"protocol"`
	}{presetHashVersion, preset, ConfirmatoryV1()}), nil
}

// LookupReferencePreset は preset を指名する sweep config の展開に使う。
func LookupReferencePreset(name string) (ReferencePreset, bool) {
	preset, ok := referencePresets()[name]
	return preset, ok
}

// NetemRegime は preset が固定する netem 条件。
func (p ReferencePreset) NetemRegime() *NetemRegime {
	return &NetemRegime{
		LinkMTUBytes:    p.LinkMTUBytes,
		DisableOffloads: p.DisableOffloads,
		ServerEgress:    p.ServerEgress,
		ClientEgress:    p.ClientEgress,
	}
}

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
	cfg.Netem = preset.NetemRegime()
	cfg.Warmup = Duration{preset.Warmup}
	cfg.SteadyWarmup = true
	cfg.Duration = Duration{preset.Duration}
	cfg.Drain = Duration{preset.Drain}
	cfg.StalenessPeriodNS = preset.StalenessPeriodNS
	return nil
}
