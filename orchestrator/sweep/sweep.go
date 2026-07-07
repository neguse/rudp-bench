package sweep

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"math/rand"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/run"
)

// TransportSpec は transport ごとの起動コマンド。args は run package の
// テンプレート変数({conns} 等)を使える。workload フラグは書かないこと
// (orchestrator が付与する)。
type TransportSpec struct {
	ServerCommand run.CommandConfig `json:"server_command"`
	ClientCommand run.CommandConfig `json:"client_command"`
	ClientProcs   int               `json:"client_procs"`
	// TCP 系(blocking send)は true: sched 遅延を farm でなく transport に帰属
	SchedIsMeasurand bool `json:"sched_is_measurand,omitempty"`
	// 定常が見えてもこれより早く窓を開かない(遅い過渡の宣言。enet は 15s)
	SteadyMinWarmup run.Duration `json:"steady_min_warmup,omitempty"`
}

type ConnsRange struct {
	Min int `json:"min"`
	Max int `json:"max"`
}

type Config struct {
	Regime     string                   `json:"regime"` // 表示・結果行用のラベル(wired 等)
	Transports map[string]TransportSpec `json:"transports"`
	Workloads  []string                 `json:"workloads"`
	Conns      ConnsRange               `json:"conns"`
	Seed       int64                    `json:"seed"`
	Warmup     run.Duration             `json:"warmup"`
	// SteadyWarmup: 定常判定つき warmup(benchspec v2)。Warmup は上限になる
	SteadyWarmup      bool             `json:"steady_warmup,omitempty"`
	Drain             run.Duration     `json:"drain"`
	Duration          run.Duration     `json:"duration,omitempty"` // 0 = loss イベント規則で自動
	DeadlineNS        uint64           `json:"deadline_ns"`
	StalenessPeriodNS uint64           `json:"staleness_period_ns"`
	Netem             *run.NetemRegime `json:"netem,omitempty"`
	ServerCPUs        string           `json:"server_cpus,omitempty"`
	ClientCPUs        string           `json:"client_cpus,omitempty"`
	OutputDir         string           `json:"output_dir"`
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
	if cfg.OutputDir == "" {
		return cfg, fmt.Errorf("output_dir is required")
	}
	if len(cfg.Transports) == 0 || len(cfg.Workloads) == 0 {
		return cfg, fmt.Errorf("transports and workloads are required")
	}
	if cfg.Regime == "" {
		return cfg, fmt.Errorf("regime label is required")
	}
	if cfg.Conns.Min == 0 {
		cfg.Conns.Min = 1
	}
	for _, name := range cfg.Workloads {
		if _, ok := run.LookupWorkload(name); !ok {
			return cfg, fmt.Errorf("unknown workload %q", name)
		}
	}
	return cfg, nil
}

// PointRecord は results.jsonl の 1 行(= 1 run)。resume の実在判定に使う。
type PointRecord struct {
	Transport string   `json:"transport"`
	Workload  string   `json:"workload"`
	Regime    string   `json:"regime"`
	Conns     int      `json:"conns"`
	Verdict   string   `json:"verdict"`
	Judgment  Judgment `json:"judgment"`
	DurationS float64  `json:"duration_s"`
	RunDir    string   `json:"run_dir"`
}

func pointKey(transport, workload, regime string, conns int) string {
	return fmt.Sprintf("%s|%s|%s|c%d", transport, workload, regime, conns)
}

// CellRecord は capacity.json の 1 行(= 1 セルの結論)。
type CellRecord struct {
	Transport string `json:"transport"`
	Workload  string `json:"workload"`
	Regime    string `json:"regime"`
	CellCapacity
}

type Sweep struct {
	cfg   Config
	cache map[string]PointRecord
	log   *os.File
	// netem 実効値 gate(ping/iperf3)は sweep 内で netem 設定が不変のため
	// 最初の実走で1回検証すれば足りる。以降の run はスキップして単価を下げる
	// (qdisc echo back は毎 run 行われる)
	gateVerified bool
}

func New(cfg Config) (*Sweep, error) {
	if err := os.MkdirAll(cfg.OutputDir, 0o755); err != nil {
		return nil, err
	}
	s := &Sweep{cfg: cfg, cache: map[string]PointRecord{}}
	if err := s.loadResume(); err != nil {
		return nil, err
	}
	f, err := os.OpenFile(filepath.Join(cfg.OutputDir, "results.jsonl"), os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644)
	if err != nil {
		return nil, err
	}
	s.log = f
	return s, nil
}

func (s *Sweep) Close() error { return s.log.Close() }

func (s *Sweep) loadResume() error {
	f, err := os.Open(filepath.Join(s.cfg.OutputDir, "results.jsonl"))
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
		if err := json.Unmarshal(sc.Bytes(), &rec); err != nil {
			continue // 途中破損行は無視(再測定される)
		}
		s.cache[pointKey(rec.Transport, rec.Workload, rec.Regime, rec.Conns)] = rec
	}
	return sc.Err()
}

// cells は seed 付き乱数でセル順をランダム化する(design spec: Randomization)。
func (s *Sweep) cells() []struct{ Transport, Workload string } {
	transports := make([]string, 0, len(s.cfg.Transports))
	for name := range s.cfg.Transports {
		transports = append(transports, name)
	}
	sort.Strings(transports)
	var out []struct{ Transport, Workload string }
	for _, t := range transports {
		for _, w := range s.cfg.Workloads {
			out = append(out, struct{ Transport, Workload string }{t, w})
		}
	}
	rng := rand.New(rand.NewSource(s.cfg.Seed))
	rng.Shuffle(len(out), func(i, j int) { out[i], out[j] = out[j], out[i] })
	return out
}

func (s *Sweep) runPoint(ctx context.Context, transport, workload string, conns int) (PointRecord, error) {
	key := pointKey(transport, workload, s.cfg.Regime, conns)
	if rec, ok := s.cache[key]; ok {
		fmt.Fprintf(os.Stderr, "[sweep] %s cached: ok=%v censored=%v %s\n", key, rec.Judgment.OK, rec.Judgment.Censored, rec.Judgment.Cause)
		return rec, nil
	}

	spec := s.cfg.Transports[transport]
	w, _ := run.LookupWorkload(workload)
	runDir := filepath.Join(s.cfg.OutputDir, "runs", transport, workload, fmt.Sprintf("c%d", conns))
	cfg := run.RunConfig{
		Transport:         transport,
		Workload:          workload,
		ServerCommand:     spec.ServerCommand,
		ClientCommand:     spec.ClientCommand,
		ClientProcs:       spec.ClientProcs,
		TotalConns:        conns,
		Warmup:            s.cfg.Warmup,
		SteadyWarmup:      s.cfg.SteadyWarmup,
		SteadyMinWarmup:   spec.SteadyMinWarmup,
		Duration:          s.cfg.Duration,
		Drain:             s.cfg.Drain,
		DeadlineNS:        s.cfg.DeadlineNS,
		StalenessPeriodNS: s.cfg.StalenessPeriodNS,
		Netem:             s.cfg.Netem,
		NetemGateOff:      s.gateVerified,
		ServerCPUs:        s.cfg.ServerCPUs,
		ClientCPUs:        s.cfg.ClientCPUs,
		SchedIsMeasurand:  spec.SchedIsMeasurand,
		OutputDir:         runDir,
	}
	cfg, err := cfg.Prepare()
	if err != nil {
		return PointRecord{}, fmt.Errorf("%s: prepare: %w", key, err)
	}

	start := time.Now()
	fmt.Fprintf(os.Stderr, "[sweep] %s running (duration=%s)...\n", key, cfg.Duration.Duration)
	result, err := run.Run(ctx, cfg)
	if err != nil {
		return PointRecord{}, fmt.Errorf("%s: run: %w", key, err)
	}

	if !s.gateVerified && result.Netem != nil && result.Netem.Gate != nil && result.Netem.Gate.OK() {
		s.gateVerified = true
	}

	rec := PointRecord{
		Transport: transport,
		Workload:  workload,
		Regime:    s.cfg.Regime,
		Conns:     conns,
		Verdict:   result.Verdict,
		Judgment:  Judge(result, w, conns, s.cfg.Netem, s.cfg.StalenessPeriodNS),
		DurationS: time.Since(start).Seconds(),
		RunDir:    runDir,
	}
	line, err := json.Marshal(rec)
	if err != nil {
		return rec, err
	}
	if _, err := s.log.Write(append(line, '\n')); err != nil {
		return rec, err
	}
	s.cache[key] = rec
	fmt.Fprintf(os.Stderr, "[sweep] %s → ok=%v censored=%v %s\n", key, rec.Judgment.OK, rec.Judgment.Censored, rec.Judgment.Cause)
	return rec, nil
}

// Run は全セルの capacity 探索を実行し、capacity.json に結論を書く。
func (s *Sweep) Run(ctx context.Context) ([]CellRecord, error) {
	var cells []CellRecord
	for _, cell := range s.cells() {
		if ctx.Err() != nil {
			return cells, ctx.Err()
		}
		eval := func(conns int) (PointOutcome, error) {
			rec, err := s.runPoint(ctx, cell.Transport, cell.Workload, conns)
			if err != nil {
				return PointOutcome{}, err
			}
			// 測定不成立(インフラ故障・環境不成立)は break でも censored でも
			// ないので1回だけ再測する(再生計画 D3)。再測も不成立なら
			// censored 扱いで打ち切り、原因に invalid を残す(誤って「server の
			// break」として capacity に載せない)
			if strings.HasPrefix(rec.Judgment.Cause, "invalid:") {
				fmt.Fprintf(os.Stderr, "[sweep] %s invalid — retrying once: %s\n",
					pointKey(cell.Transport, cell.Workload, s.cfg.Regime, conns), rec.Judgment.Cause)
				delete(s.cache, pointKey(cell.Transport, cell.Workload, s.cfg.Regime, conns))
				rec, err = s.runPoint(ctx, cell.Transport, cell.Workload, conns)
				if err != nil {
					return PointOutcome{}, err
				}
				if strings.HasPrefix(rec.Judgment.Cause, "invalid:") {
					return PointOutcome{Censored: true, Cause: "measurement_invalid: " + rec.Judgment.Cause}, nil
				}
			}
			return PointOutcome{OK: rec.Judgment.OK, Censored: rec.Judgment.Censored, Cause: rec.Judgment.Cause}, nil
		}
		cap, err := FindCapacity(eval, s.cfg.Conns.Min, s.cfg.Conns.Max)
		if err != nil {
			return cells, fmt.Errorf("cell %s/%s: %w", cell.Transport, cell.Workload, err)
		}
		rec := CellRecord{Transport: cell.Transport, Workload: cell.Workload, Regime: s.cfg.Regime, CellCapacity: cap}
		cells = append(cells, rec)
		fmt.Fprintf(os.Stderr, "[sweep] CELL %s/%s/%s capacity=%d censored=%v range_limited=%v break=%d cause=%s\n",
			cell.Transport, cell.Workload, s.cfg.Regime, cap.Capacity, cap.Censored, cap.RangeLimited, cap.BreakConns, cap.BreakCause)
		if err := s.writeCells(cells); err != nil {
			return cells, err
		}
	}
	return cells, nil
}

func (s *Sweep) writeCells(cells []CellRecord) error {
	data, err := json.MarshalIndent(struct {
		Seed  int64        `json:"seed"`
		Cells []CellRecord `json:"cells"`
	}{s.cfg.Seed, cells}, "", " ")
	if err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(s.cfg.OutputDir, "capacity.json"), append(data, '\n'), 0o644)
}
