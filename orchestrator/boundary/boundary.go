// Package boundary は主張2のスライス driver: loss 平面(loss% × burst 長)を
// anchor workload × 負荷アンカー(無負荷極限 / capacity@wired の割合)で掃き、
// staleness p99 を一次指標として記録する。capacity sweep と違い conns は固定で
// 探索しない(応答曲面の別断面)。
package boundary

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"math/rand"
	"os"
	"path/filepath"
	"sort"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/netops"
	"github.com/neguse/rudp-bench/orchestrator/run"
	"github.com/neguse/rudp-bench/orchestrator/sweep"
)

type Config struct {
	DelayMS int `json:"delay_ms"`
	// LossSeedBase > 0 で決定的 loss 注入(netops/losstrace)。セルごとに
	// (loss, burst) から導出した seed を使う — 同じセルは全 transport・全 run で
	// 同じ落ち方を再生する(再生計画 D2)
	LossSeedBase uint64    `json:"loss_seed_base,omitempty"`
	LossPcts     []float64 `json:"loss_pcts"`
	BurstLens    []float64 `json:"burst_lens"`
	Anchors      []string  `json:"anchors"`
	// 負荷アンカー: FloorConns = 無負荷極限、Fractions = capacity@wired 比
	FloorConns int       `json:"floor_conns"`
	Fractions  []float64 `json:"fractions"`
	// capacity@wired の出典(sweep 出力の capacity.json)
	CapacityJSON string                         `json:"capacity_json"`
	Transports   map[string]sweep.TransportSpec `json:"transports"`
	Seed         int64                          `json:"seed"`
	Warmup       run.Duration                   `json:"warmup"`
	// SteadyWarmup: 定常判定つき warmup(benchspec v2)。Warmup は上限になる
	SteadyWarmup      bool         `json:"steady_warmup,omitempty"`
	Drain             run.Duration `json:"drain"`
	DeadlineNS        uint64       `json:"deadline_ns"`
	StalenessPeriodNS uint64       `json:"staleness_period_ns"`
	ServerCPUs        string       `json:"server_cpus,omitempty"`
	ClientCPUs        string       `json:"client_cpus,omitempty"`
	OutputDir         string       `json:"output_dir"`
}

func LoadConfig(path string) (Config, error) {
	var cfg Config
	data, err := os.ReadFile(path)
	if err != nil {
		return cfg, err
	}
	if err := json.Unmarshal(data, &cfg); err != nil {
		return cfg, err
	}
	switch {
	case cfg.OutputDir == "":
		return cfg, fmt.Errorf("output_dir is required")
	case len(cfg.Transports) == 0 || len(cfg.Anchors) == 0:
		return cfg, fmt.Errorf("transports and anchors are required")
	case len(cfg.LossPcts) == 0 || len(cfg.BurstLens) == 0:
		return cfg, fmt.Errorf("loss_pcts and burst_lens are required")
	case cfg.CapacityJSON == "" && len(cfg.Fractions) > 0:
		return cfg, fmt.Errorf("capacity_json is required when fractions are set")
	}
	if cfg.FloorConns == 0 {
		cfg.FloorConns = 4
	}
	for _, a := range cfg.Anchors {
		if _, ok := run.LookupWorkload(a); !ok {
			return cfg, fmt.Errorf("unknown anchor workload %q", a)
		}
	}
	return cfg, nil
}

// capacityMap は capacity.json から (transport, workload) → cell を引く。
func loadCapacity(path string) (map[string]sweep.CellRecord, error) {
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

// Load は1点の負荷アンカー。Label は "floor" / "q25" / "q75" 等。
type Load struct {
	Label string `json:"label"`
	Conns int    `json:"conns"`
	// CapacityCensored: 基準にした capacity@wired が censored(下限値)だった
	CapacityCensored bool `json:"capacity_censored,omitempty"`
}

// loadsFor は transport × anchor の負荷アンカー列を導出する。
// capacity が floor 以下・0 の場合は割合負荷を張れない(floor のみ)。
func loadsFor(cfg Config, capacity map[string]sweep.CellRecord, transport, anchor string) []Load {
	loads := []Load{{Label: "floor", Conns: cfg.FloorConns}}
	cell, ok := capacity[transport+"|"+anchor]
	if !ok || cell.Capacity <= cfg.FloorConns {
		return loads
	}
	for _, f := range cfg.Fractions {
		conns := int(f*float64(cell.Capacity) + 0.5)
		if conns <= cfg.FloorConns {
			continue
		}
		loads = append(loads, Load{
			Label:            fmt.Sprintf("q%d", int(f*100+0.5)),
			Conns:            conns,
			CapacityCensored: cell.Censored,
		})
	}
	return loads
}

// PointRecord は results.jsonl の1行。resume の実在判定に使う。
type PointRecord struct {
	Transport string         `json:"transport"`
	Anchor    string         `json:"anchor"`
	LossPct   float64        `json:"loss_pct"`
	BurstLen  float64        `json:"burst_len"`
	Load      Load           `json:"load"`
	Verdict   string         `json:"verdict"`
	Judgment  sweep.Judgment `json:"judgment"`
	DurationS float64        `json:"duration_s"`
	RunDir    string         `json:"run_dir"`
}

func pointKey(transport, anchor string, loss, burst float64, label string, conns int) string {
	return fmt.Sprintf("%s|%s|l%gb%g|%s|c%d", transport, anchor, loss, burst, label, conns)
}

type Boundary struct {
	cfg      Config
	capacity map[string]sweep.CellRecord
	cache    map[string]PointRecord
	log      *os.File
	// netem 実効値 gate は (loss, burst) の組ごとに1回で足りる
	gateDone map[string]bool
}

func New(cfg Config) (*Boundary, error) {
	if err := os.MkdirAll(cfg.OutputDir, 0o755); err != nil {
		return nil, err
	}
	b := &Boundary{cfg: cfg, cache: map[string]PointRecord{}, gateDone: map[string]bool{}}
	if cfg.CapacityJSON != "" {
		cap, err := loadCapacity(cfg.CapacityJSON)
		if err != nil {
			return nil, err
		}
		b.capacity = cap
	}
	if err := b.loadResume(); err != nil {
		return nil, err
	}
	f, err := os.OpenFile(filepath.Join(cfg.OutputDir, "results.jsonl"), os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644)
	if err != nil {
		return nil, err
	}
	b.log = f
	return b, nil
}

func (b *Boundary) Close() error { return b.log.Close() }

func (b *Boundary) loadResume() error {
	f, err := os.Open(filepath.Join(b.cfg.OutputDir, "results.jsonl"))
	if os.IsNotExist(err) {
		return nil
	}
	if err != nil {
		return err
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 1<<20), 1<<20)
	for sc.Scan() {
		var rec PointRecord
		if json.Unmarshal(sc.Bytes(), &rec) != nil {
			continue
		}
		b.cache[pointKey(rec.Transport, rec.Anchor, rec.LossPct, rec.BurstLen, rec.Load.Label, rec.Load.Conns)] = rec
	}
	return sc.Err()
}

type cell struct{ Transport, Anchor string }

func (b *Boundary) cells() []cell {
	transports := make([]string, 0, len(b.cfg.Transports))
	for name := range b.cfg.Transports {
		transports = append(transports, name)
	}
	sort.Strings(transports)
	var out []cell
	for _, t := range transports {
		for _, a := range b.cfg.Anchors {
			out = append(out, cell{t, a})
		}
	}
	rng := rand.New(rand.NewSource(b.cfg.Seed))
	rng.Shuffle(len(out), func(i, j int) { out[i], out[j] = out[j], out[i] })
	return out
}

func (b *Boundary) netemFor(loss, burst float64) *run.NetemRegime {
	egress := netops.Netem{DelayMS: b.cfg.DelayMS, LossPercent: loss, LossBurstLen: burst}
	server, client := egress, egress
	if b.cfg.LossSeedBase > 0 && loss > 0 {
		// (loss, burst) ごとに固定の seed 対。cell 内の全 transport・全 load で
		// 同じ trace が再生される(cross-transport 比較から乱数差が消える)
		cell := uint64(loss*100)*1000 + uint64(burst*10)
		server.LossSeed = b.cfg.LossSeedBase + cell*2
		client.LossSeed = b.cfg.LossSeedBase + cell*2 + 1
	}
	return &run.NetemRegime{ServerEgress: server, ClientEgress: client}
}

func (b *Boundary) runPoint(ctx context.Context, c cell, loss, burst float64, load Load) (PointRecord, error) {
	key := pointKey(c.Transport, c.Anchor, loss, burst, load.Label, load.Conns)
	if rec, ok := b.cache[key]; ok {
		fmt.Fprintf(os.Stderr, "[boundary] %s cached: p99=%dms\n", key, rec.Judgment.StalenessP99/1_000_000)
		return rec, nil
	}
	spec := b.cfg.Transports[c.Transport]
	w, _ := run.LookupWorkload(c.Anchor)
	netem := b.netemFor(loss, burst)
	gateKey := fmt.Sprintf("l%gb%g", loss, burst)
	runDir := filepath.Join(b.cfg.OutputDir, "runs", c.Transport, c.Anchor,
		fmt.Sprintf("l%gb%g-%s-c%d", loss, burst, load.Label, load.Conns))

	cfg := run.RunConfig{
		Transport:         c.Transport,
		Workload:          c.Anchor,
		ServerCommand:     spec.ServerCommand,
		ClientCommand:     spec.ClientCommand,
		ClientProcs:       spec.ClientProcs,
		TotalConns:        load.Conns,
		Warmup:            b.cfg.Warmup,
		SteadyWarmup:      b.cfg.SteadyWarmup,
		SteadyMinWarmup:   spec.SteadyMinWarmup,
		Drain:             b.cfg.Drain,
		DeadlineNS:        b.cfg.DeadlineNS,
		StalenessPeriodNS: b.cfg.StalenessPeriodNS,
		Netem:             netem,
		NetemGateOff:      b.gateDone[gateKey],
		ServerCPUs:        b.cfg.ServerCPUs,
		ClientCPUs:        b.cfg.ClientCPUs,
		SchedIsMeasurand:  spec.SchedIsMeasurand,
		OutputDir:         runDir,
	}
	cfg, err := cfg.Prepare()
	if err != nil {
		return PointRecord{}, fmt.Errorf("%s: prepare: %w", key, err)
	}

	start := time.Now()
	fmt.Fprintf(os.Stderr, "[boundary] %s running (duration=%s)...\n", key, cfg.Duration.Duration)
	result, err := run.Run(ctx, cfg)
	if err != nil {
		return PointRecord{}, fmt.Errorf("%s: run: %w", key, err)
	}
	if !b.gateDone[gateKey] && result.Netem != nil && result.Netem.Gate != nil && result.Netem.Gate.OK() {
		b.gateDone[gateKey] = true
	}

	rec := PointRecord{
		Transport: c.Transport,
		Anchor:    c.Anchor,
		LossPct:   loss,
		BurstLen:  burst,
		Load:      load,
		Verdict:   result.Verdict,
		Judgment:  sweep.Judge(result, w, load.Conns, netem, b.cfg.StalenessPeriodNS),
		DurationS: time.Since(start).Seconds(),
		RunDir:    runDir,
	}
	line, err := json.Marshal(rec)
	if err != nil {
		return rec, err
	}
	if _, err := b.log.Write(append(line, '\n')); err != nil {
		return rec, err
	}
	b.cache[key] = rec
	fmt.Fprintf(os.Stderr, "[boundary] %s → %s p99=%dms floor=%dms %s\n",
		key, rec.Verdict, rec.Judgment.StalenessP99/1_000_000, rec.Judgment.FloorStaleNS/1_000_000, rec.Judgment.Cause)
	return rec, nil
}

// Run は全セルを実行し、boundary.json に全点を書く。
func (b *Boundary) Run(ctx context.Context) ([]PointRecord, error) {
	var all []PointRecord
	for _, c := range b.cells() {
		loads := loadsFor(b.cfg, b.capacity, c.Transport, c.Anchor)
		for _, load := range loads {
			for _, loss := range b.cfg.LossPcts {
				for _, burst := range b.cfg.BurstLens {
					if ctx.Err() != nil {
						return all, ctx.Err()
					}
					rec, err := b.runPoint(ctx, c, loss, burst, load)
					if err != nil {
						return all, err
					}
					all = append(all, rec)
				}
			}
		}
		if err := b.writeAll(all); err != nil {
			return all, err
		}
	}
	return all, nil
}

func (b *Boundary) writeAll(points []PointRecord) error {
	data, err := json.MarshalIndent(struct {
		Seed   int64         `json:"seed"`
		Points []PointRecord `json:"points"`
	}{b.cfg.Seed, points}, "", " ")
	if err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(b.cfg.OutputDir, "boundary.json"), append(data, '\n'), 0o644)
}
