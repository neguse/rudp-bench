// Package sentinel は日常運用ティア(~1h)の回帰検知 driver。
// 基準ブロックの結果から番兵点を選び、探索なしの少数プローブで
// 「基準から漂移していないか」だけを機械判定する。
// 統計ティア(block)と違い duration は短い固定値 — 精度ではなく
// ドリフト検知が目的であることをレポートに開示する。
package sentinel

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/rig"
	"github.com/neguse/rudp-bench/orchestrator/run"
	"github.com/neguse/rudp-bench/orchestrator/sweep"
)

type RegimeSpec struct {
	Name  string           `json:"name"`
	Netem *run.NetemRegime `json:"netem,omitempty"`
	// ReferenceDir: 基準ブロック内の該当 sweep ディレクトリ
	ReferenceDir string `json:"reference_dir"`
}

type Config struct {
	Rig        string                         `json:"rig"`
	Transports map[string]sweep.TransportSpec `json:"transports"`
	// 番兵にする workload(通常は anchor 3 セル)
	Workloads []string     `json:"workloads"`
	Regimes   []RegimeSpec `json:"regimes"`
	// プローブの計測時間(短い固定値。既定 30s)
	Duration          run.Duration `json:"duration,omitempty"`
	Warmup            run.Duration `json:"warmup"`
	Drain             run.Duration `json:"drain"`
	DeadlineNS        uint64       `json:"deadline_ns"`
	StalenessPeriodNS uint64       `json:"staleness_period_ns"`
	OutputDir         string       `json:"output_dir"`
}

func LoadConfig(path string) (Config, error) {
	var cfg Config
	data, err := os.ReadFile(path)
	if err != nil {
		return cfg, err
	}
	if err := json.Unmarshal(data, &cfg); err != nil {
		return cfg, fmt.Errorf("%s: %w", path, err)
	}
	if cfg.OutputDir == "" || cfg.Rig == "" || len(cfg.Transports) == 0 ||
		len(cfg.Workloads) == 0 || len(cfg.Regimes) == 0 {
		return cfg, fmt.Errorf("rig, transports, workloads, regimes, output_dir are required")
	}
	if cfg.Duration.Duration == 0 {
		cfg.Duration.Duration = 30 * time.Second
	}
	return cfg, nil
}

// Probe は1番兵点の計画と結果。
type Probe struct {
	Transport string `json:"transport"`
	Workload  string `json:"workload"`
	Regime    string `json:"regime"`
	Conns     int    `json:"conns"`
	// Expect: "ok"(基準 capacity 点)/ "fail"(基準 break 点)/
	// "bound"(censored 下限点 — OK でも censored でも drift としない)
	Expect string `json:"expect"`

	Verdict  string         `json:"verdict,omitempty"`
	Judgment sweep.Judgment `json:"judgment,omitempty"`
	Outcome  string         `json:"outcome,omitempty"` // PASS / DRIFT / ERROR
	Note     string         `json:"note,omitempty"`
}

// PlanProbes は基準 capacity.json から番兵点を導く:
// - 正直な break セル → capacity の 0.9 倍(expect ok)と break の 1.15 倍
//   (expect fail)の2点。**際そのものは突かない** — 境界上の点は run 間の
//   自然なゆらぎ(IQR)で毎回パタつくため、日常スモークの感度は
//   「±10-15% を超える漂移の検知」に置く
// - censored / range-limited セル → 下限点1点(expect bound)
// - capacity 0 のセル → break 点(= min probe)1点(expect fail)
func PlanProbes(cfg Config) ([]Probe, error) {
	var probes []Probe
	for _, reg := range cfg.Regimes {
		cells, err := loadCapacityCells(filepath.Join(reg.ReferenceDir, "capacity.json"))
		if err != nil {
			return nil, fmt.Errorf("regime %s: %w", reg.Name, err)
		}
		for _, w := range cfg.Workloads {
			for transport := range cfg.Transports {
				cell, ok := cells[transport+"|"+w]
				if !ok {
					continue
				}
				switch {
				case cell.Censored || cell.RangeLimited:
					if cell.Capacity > 0 {
						probes = append(probes, Probe{Transport: transport, Workload: w,
							Regime: reg.Name, Conns: cell.Capacity, Expect: "bound"})
					}
				case cell.Capacity == 0:
					conns := cell.BreakConns
					if conns == 0 {
						conns = 1
					}
					probes = append(probes, Probe{Transport: transport, Workload: w,
						Regime: reg.Name, Conns: conns, Expect: "fail"})
				default:
					okConns := int(float64(cell.Capacity)*0.9 + 0.5)
					if okConns < 1 {
						okConns = 1
					}
					probes = append(probes, Probe{Transport: transport, Workload: w,
						Regime: reg.Name, Conns: okConns, Expect: "ok"})
					if cell.BreakConns > 0 {
						failConns := int(float64(cell.BreakConns)*1.15 + 0.5)
						if failConns <= okConns {
							failConns = okConns + 1
						}
						probes = append(probes, Probe{Transport: transport, Workload: w,
							Regime: reg.Name, Conns: failConns, Expect: "fail"})
					}
				}
			}
		}
	}
	return probes, nil
}

func loadCapacityCells(path string) (map[string]sweep.CellRecord, error) {
	var doc struct {
		Cells []sweep.CellRecord `json:"cells"`
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	if err := json.Unmarshal(data, &doc); err != nil {
		return nil, fmt.Errorf("%s: %w", path, err)
	}
	out := map[string]sweep.CellRecord{}
	for _, c := range doc.Cells {
		out[c.Transport+"|"+c.Workload] = c
	}
	return out, nil
}

// judgeProbe は実測と期待の突き合わせ。
func judgeProbe(p *Probe) {
	j := p.Judgment
	switch p.Expect {
	case "ok":
		if j.OK {
			p.Outcome = "PASS"
		} else if j.Censored {
			p.Outcome = "DRIFT"
			p.Note = "capacity 点が farm 打ち切りに変化(計測器側の漂移疑い)"
		} else {
			p.Outcome = "DRIFT"
			p.Note = "基準 capacity 点で break: " + j.Cause
		}
	case "fail":
		if !j.OK && !j.Censored {
			p.Outcome = "PASS"
		} else if j.Censored {
			p.Outcome = "DRIFT"
			p.Note = "基準 break 点が censored に変化: " + j.Cause
		} else {
			p.Outcome = "DRIFT"
			p.Note = "基準 break 点が OK に変化(capacity 上振れ — 改善の可能性)"
		}
	case "bound":
		// 下限点は OK / censored のどちらでも矛盾しない。正直な break だけ漂移
		if j.OK || j.Censored {
			p.Outcome = "PASS"
		} else {
			p.Outcome = "DRIFT"
			p.Note = "基準下限点で break: " + j.Cause
		}
	default:
		p.Outcome = "ERROR"
		p.Note = "unknown expect: " + p.Expect
	}
}

// Run は番兵点を順に実行し、sentinel.json と PASS/DRIFT 表を出力する。
func Run(ctx context.Context, cfg Config) (int, error) {
	r, err := rig.Load(cfg.Rig)
	if err != nil {
		return 0, err
	}
	probes, err := PlanProbes(cfg)
	if err != nil {
		return 0, err
	}
	if err := os.MkdirAll(cfg.OutputDir, 0o755); err != nil {
		return 0, err
	}

	regimes := map[string]RegimeSpec{}
	for _, reg := range cfg.Regimes {
		regimes[reg.Name] = reg
	}
	gateDone := map[string]bool{}

	drift := 0
	for i := range probes {
		p := &probes[i]
		if ctx.Err() != nil {
			return drift, ctx.Err()
		}
		spec := cfg.Transports[p.Transport]
		reg := regimes[p.Regime]
		runDir := filepath.Join(cfg.OutputDir, "runs",
			fmt.Sprintf("%s-%s-%s-c%d", p.Regime, p.Transport, p.Workload, p.Conns))
		rcfg := run.RunConfig{
			Transport:         p.Transport,
			Workload:          p.Workload,
			ServerCommand:     spec.ServerCommand,
			ClientCommand:     spec.ClientCommand,
			ClientProcs:       spec.ClientProcs,
			SchedIsMeasurand:  spec.SchedIsMeasurand,
			TotalConns:        p.Conns,
			Warmup:            cfg.Warmup,
			Duration:          cfg.Duration,
			Drain:             cfg.Drain,
			DeadlineNS:        cfg.DeadlineNS,
			StalenessPeriodNS: cfg.StalenessPeriodNS,
			Netem:             reg.Netem,
			NetemGateOff:      gateDone[p.Regime],
			ServerCPUs:        r.ServerCPUs,
			ClientCPUs:        r.ClientCPUs,
			OutputDir:         runDir,
		}
		rcfg, err := rcfg.Prepare()
		if err != nil {
			p.Outcome, p.Note = "ERROR", err.Error()
			drift++
			continue
		}
		fmt.Fprintf(os.Stderr, "[sentinel] %s/%s/%s c%d (expect %s)...\n",
			p.Regime, p.Transport, p.Workload, p.Conns, p.Expect)
		result, err := run.Run(ctx, rcfg)
		if err != nil {
			p.Outcome, p.Note = "ERROR", err.Error()
			drift++
			continue
		}
		if reg.Netem != nil && result.Netem != nil && result.Netem.Gate != nil && result.Netem.Gate.OK() {
			gateDone[p.Regime] = true
		}
		w, _ := run.LookupWorkload(p.Workload)
		p.Verdict = result.Verdict
		p.Judgment = sweep.Judge(result, w, p.Conns, reg.Netem, cfg.StalenessPeriodNS)
		judgeProbe(p)
		if p.Outcome != "PASS" {
			drift++
		}
		fmt.Fprintf(os.Stderr, "[sentinel] → %s %s\n", p.Outcome, p.Note)
	}

	data, err := json.MarshalIndent(struct {
		At     time.Time `json:"at"`
		Probes []Probe   `json:"probes"`
	}{time.Now(), probes}, "", " ")
	if err != nil {
		return drift, err
	}
	if err := os.WriteFile(filepath.Join(cfg.OutputDir, "sentinel.json"), append(data, '\n'), 0o644); err != nil {
		return drift, err
	}

	fmt.Println(RenderTable(probes))
	return drift, nil
}

// RenderTable は PASS/DRIFT の一覧表。
func RenderTable(probes []Probe) string {
	out := "| regime | transport | workload | conns | expect | outcome | note |\n|---|---|---|---|---|---|---|\n"
	for _, p := range probes {
		out += fmt.Sprintf("| %s | %s | %s | %d | %s | %s | %s |\n",
			p.Regime, p.Transport, p.Workload, p.Conns, p.Expect, p.Outcome, p.Note)
	}
	return out
}
