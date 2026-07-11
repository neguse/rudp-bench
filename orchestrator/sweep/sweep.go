package sweep

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"math/rand"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/doctor"
	"github.com/neguse/rudp-bench/orchestrator/run"
)

// TransportSpec は transport ごとの起動コマンド。args は run package の
// テンプレート変数({conns} 等)を使える。workload フラグは書かないこと
// (orchestrator が付与する)。
type TransportSpec struct {
	ServerCommand run.CommandConfig `json:"server_command"`
	ClientCommand run.CommandConfig `json:"client_command"`
	ClientProcs   int               `json:"client_procs"`
	// ClassMappingSHA256 is filled by Sweep.New after endpoint preflight. When
	// supplied in config it is an expected value and must match the preflight.
	ClassMappingSHA256 string `json:"class_mapping_sha256,omitempty"`
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
	MeasurementMode string                   `json:"measurement_mode,omitempty"`
	DoctorReport    string                   `json:"doctor_report,omitempty"`
	Regime          string                   `json:"regime"` // 表示・結果行用のラベル(wired 等)
	Transports      map[string]TransportSpec `json:"transports"`
	Workloads       []string                 `json:"workloads"`
	Scenarios       []run.ScenarioSpec       `json:"scenarios,omitempty"`
	Conns           ConnsRange               `json:"conns"`
	Seed            int64                    `json:"seed"`
	Warmup          run.Duration             `json:"warmup"`
	// SteadyWarmup: 定常判定つき warmup(benchspec v2)。Warmup は上限になる
	SteadyWarmup      bool             `json:"steady_warmup,omitempty"`
	Drain             run.Duration     `json:"drain"`
	Duration          run.Duration     `json:"duration,omitempty"` // 0 = loss イベント規則で自動
	DeadlineNS        uint64           `json:"deadline_ns"`
	StalenessPeriodNS uint64           `json:"staleness_period_ns"`
	Netem             *run.NetemRegime `json:"netem,omitempty"`
	ServerCPUs        string           `json:"server_cpus,omitempty"`
	ClientCPUs        string           `json:"client_cpus,omitempty"`
	// Baseline は block 前後の environment baseline と drift 許容幅。
	// reference mode では必須(ADR-0002 の block gate)
	Baseline  *BaselineSpec `json:"baseline,omitempty"`
	OutputDir string        `json:"output_dir"`
}

func LoadConfig(path string) (Config, error) {
	var cfg Config
	data, err := os.ReadFile(path)
	if err != nil {
		return cfg, err
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&cfg); err != nil {
		return cfg, err
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		return cfg, fmt.Errorf("config must contain exactly one JSON object")
	}
	if cfg.OutputDir == "" {
		return cfg, fmt.Errorf("output_dir is required")
	}
	if cfg.MeasurementMode == "" {
		cfg.MeasurementMode = "screening"
	}
	switch cfg.MeasurementMode {
	case "conformance", "screening", "pilot":
		if cfg.DoctorReport != "" {
			if _, err := doctor.Read(cfg.DoctorReport); err != nil {
				return cfg, fmt.Errorf("doctor_report: %w", err)
			}
		}
	case "reference":
		if cfg.Baseline == nil {
			return cfg, fmt.Errorf("reference mode requires a baseline block with drift tolerances")
		}
		if cfg.DoctorReport == "" {
			return cfg, fmt.Errorf("reference mode requires doctor_report")
		}
		report, err := doctor.Read(cfg.DoctorReport)
		if err != nil {
			return cfg, fmt.Errorf("doctor_report: %w", err)
		}
		if err := doctor.ValidateReferenceReport(report, time.Now().UTC()); err != nil {
			return cfg, fmt.Errorf("reference preflight: %w", err)
		}
		if cfg.Conns.Min != 0 && cfg.Conns.Min != 1 {
			return cfg, fmt.Errorf("reference capacity search must start at conns.min=1 to avoid left censoring")
		}
		if cfg.Netem != nil && (cfg.Netem.ClientEgress.LossSeed != 0 || cfg.Netem.ServerEgress.LossSeed != 0) {
			return cfg, fmt.Errorf("reference mode does not yet accept loss_seed: deterministic trace delivery needs a known-packet conformance gate")
		}
	default:
		return cfg, fmt.Errorf("measurement_mode must be conformance, screening, pilot, or reference")
	}
	if len(cfg.Transports) == 0 {
		return cfg, fmt.Errorf("transports are required")
	}
	if cfg.Baseline != nil {
		if err := cfg.Baseline.validate(); err != nil {
			return cfg, err
		}
	}
	if (len(cfg.Workloads) == 0) == (len(cfg.Scenarios) == 0) {
		return cfg, fmt.Errorf("exactly one of workloads or scenarios is required")
	}
	if !run.IsSafeName(cfg.Regime) {
		return cfg, fmt.Errorf("regime must be a path-safe ASCII slug")
	}
	if cfg.Conns.Min == 0 {
		cfg.Conns.Min = 1
	}
	if cfg.Conns.Max < cfg.Conns.Min {
		return cfg, fmt.Errorf("conns range [%d,%d] is invalid", cfg.Conns.Min, cfg.Conns.Max)
	}
	seenWorkloads := map[string]bool{}
	for _, name := range cfg.Workloads {
		if _, ok := run.LookupWorkload(name); !ok {
			return cfg, fmt.Errorf("unknown workload %q", name)
		}
		if seenWorkloads[name] {
			return cfg, fmt.Errorf("duplicate workload %q", name)
		}
		seenWorkloads[name] = true
	}
	seenScenarios := map[string]bool{}
	for name, transport := range cfg.Transports {
		if !run.IsSafeName(name) {
			return cfg, fmt.Errorf("transport name %q must be a path-safe ASCII slug", name)
		}
		if transport.ServerCommand.Path == "" || transport.ClientCommand.Path == "" || transport.ClientProcs <= 0 {
			return cfg, fmt.Errorf("transport %q requires server/client command and client_procs > 0", name)
		}
	}
	for i := range cfg.Scenarios {
		if err := cfg.Scenarios[i].Validate(); err != nil {
			return cfg, fmt.Errorf("scenarios[%d]: %w", i, err)
		}
		if seenScenarios[cfg.Scenarios[i].Name] {
			return cfg, fmt.Errorf("duplicate scenario name %q", cfg.Scenarios[i].Name)
		}
		if missing := cfg.Scenarios[i].MissingPrimarySLOs(); len(missing) > 0 {
			return cfg, fmt.Errorf("scenarios[%d] cannot be used for capacity search: primary SLOs missing for %s", i, strings.Join(missing, ", "))
		}
		seenScenarios[cfg.Scenarios[i].Name] = true
	}
	return cfg, nil
}

// PointRecord は results.jsonl の 1 行(= 1 run)。resume の実在判定に使う。
type PointRecord struct {
	Transport                string      `json:"transport"`
	Workload                 string      `json:"workload,omitempty"`
	Scenario                 string      `json:"scenario,omitempty"`
	Regime                   string      `json:"regime"`
	Conns                    int         `json:"conns"`
	Verdict                  string      `json:"verdict"`
	Outcome                  run.Outcome `json:"outcome"`
	Judgment                 Judgment    `json:"judgment"`
	DurationS                float64     `json:"duration_s"`
	RunDir                   string      `json:"run_dir"`
	RunIdentity              string      `json:"run_identity,omitempty"`
	AcquisitionID            string      `json:"acquisition_id,omitempty"`
	Attempt                  int         `json:"attempt,omitempty"`
	CampaignIdentity         string      `json:"campaign_identity,omitempty"`
	ComparisonIdentity       string      `json:"comparison_identity,omitempty"`
	MeasurementInvalid       bool        `json:"measurement_invalid,omitempty"`
	Rejudged                 bool        `json:"rejudged,omitempty"`
	AnalysisIdentity         string      `json:"analysis_identity,omitempty"`
	SourceResultSHA256       string      `json:"source_result_sha256,omitempty"`
	SourceJudgment           *Judgment   `json:"source_judgment,omitempty"`
	SourceComparisonIdentity string      `json:"source_comparison_identity,omitempty"`
	SourceMeasurementInvalid *bool       `json:"source_measurement_invalid,omitempty"`
}

func pointKey(transport, workload, regime string, conns int) string {
	return fmt.Sprintf("%s|%s|%s|c%d", transport, workload, regime, conns)
}

// CellRecord は capacity.json の 1 行(= 1 セルの結論)。
type CellRecord struct {
	Transport          string   `json:"transport"`
	Workload           string   `json:"workload,omitempty"`
	Scenario           string   `json:"scenario,omitempty"`
	CampaignIdentity   string   `json:"campaign_identity,omitempty"`
	ComparisonIdentity string   `json:"comparison_identity,omitempty"`
	EvidenceIDs        []string `json:"evidence_ids,omitempty"`
	AnalysisIdentity   string   `json:"analysis_identity,omitempty"`
	Regime             string   `json:"regime"`
	// BlockInvalid: block 前後 baseline の drift gate を外れた。SUT の break で
	// はなく、数値 aggregate へは載せない(ADR-0002)
	BlockInvalid      bool   `json:"block_invalid,omitempty"`
	BlockInvalidCause string `json:"block_invalid_cause,omitempty"`
	CellCapacity
}

type cellDefinition struct {
	Transport string
	Workload  string
	Scenario  *run.ScenarioSpec
}

func (c cellDefinition) name() string {
	if c.Scenario != nil {
		return c.Scenario.Name
	}
	return c.Workload
}

func (r PointRecord) testName() string {
	if r.Scenario != "" {
		return r.Scenario
	}
	return r.Workload
}

func (r PointRecord) TestName() string { return r.testName() }

func (c CellRecord) TestName() string {
	if c.Scenario != "" {
		return c.Scenario
	}
	return c.Workload
}

type Sweep struct {
	cfg              Config
	cache            map[string]PointRecord
	log              *os.File
	campaignIdentity string
	attempts         map[string]int
	gateVerified     bool // retained for old result compatibility; fresh setups always gate
	block            *BlockBaseline
}

func New(cfg Config) (*Sweep, error) {
	if err := os.MkdirAll(cfg.OutputDir, 0o755); err != nil {
		return nil, err
	}
	for name, transport := range cfg.Transports {
		mapping, err := run.PreflightClassMapping(context.Background(), name, transport.ServerCommand, transport.ClientCommand)
		if err != nil {
			return nil, fmt.Errorf("transport %q class_mapping preflight: %w", name, err)
		}
		actual := mapping.ServerSHA256
		if transport.ClassMappingSHA256 != "" && transport.ClassMappingSHA256 != actual {
			return nil, fmt.Errorf("transport %q class_mapping_sha256=%q, preflight=%q", name, transport.ClassMappingSHA256, actual)
		}
		transport.ClassMappingSHA256 = actual
		cfg.Transports[name] = transport
	}
	binaries := map[string][2]string{}
	for name, transport := range cfg.Transports {
		binaries[name] = [2]string{
			run.CommandFingerprint(transport.ServerCommand),
			run.CommandFingerprint(transport.ClientCommand),
		}
	}
	campaignIdentity := sweepCampaignIdentity(cfg, binaries)
	s := &Sweep{cfg: cfg, cache: map[string]PointRecord{}, attempts: map[string]int{}, campaignIdentity: campaignIdentity}
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

func sweepCampaignIdentity(cfg Config, binaries map[string][2]string) string {
	identityConfig := cfg
	identityConfig.OutputDir = ""
	identityConfig.DoctorReport = ""
	return run.HashValue(struct {
		Config             Config               `json:"config"`
		Binaries           map[string][2]string `json:"binaries"`
		OrchestratorSHA256 string               `json:"orchestrator_sha256,omitempty"`
		EnvironmentSHA256  string               `json:"environment_sha256"`
		DoctorSHA256       string               `json:"doctor_sha256,omitempty"`
	}{identityConfig, binaries, run.OrchestratorFingerprint(), run.EnvironmentFingerprint(), doctorReportFingerprint(cfg.DoctorReport)})
}

func (s *Sweep) Close() error { return s.log.Close() }

func (s *Sweep) loadResume() error {
	if s.attempts == nil {
		s.attempts = map[string]int{}
	}
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
	latest := map[string]PointRecord{}
	for sc.Scan() {
		var rec PointRecord
		if err := json.Unmarshal(sc.Bytes(), &rec); err != nil {
			continue // 途中破損行は無視(再測定される)
		}
		if rec.RunIdentity != "" {
			latest[rec.RunIdentity] = rec
			attempt := rec.Attempt
			if attempt == 0 {
				attempt = 1
			}
			if attempt > s.attempts[rec.RunIdentity] {
				s.attempts[rec.RunIdentity] = attempt
			}
		}
	}
	if err := sc.Err(); err != nil {
		return err
	}
	for runIdentity, rec := range latest {
		if s.cacheRecordEligible(rec) {
			s.cache[runIdentity] = rec
		}
	}
	return nil
}

func (s *Sweep) cacheRecordEligible(rec PointRecord) bool {
	if rec.Rejudged {
		return false
	}
	if rec.CampaignIdentity != s.campaignIdentity {
		return false
	}
	return true
}

func doctorReportFingerprint(path string) string {
	if path == "" {
		return ""
	}
	report, err := doctor.Read(path)
	if err != nil {
		return "unreadable"
	}
	report.GeneratedAt = time.Time{}
	return run.HashValue(report)
}

// cells は seed 付き乱数でセル順をランダム化する(design spec: Randomization)。
func (s *Sweep) cells() []cellDefinition {
	transports := make([]string, 0, len(s.cfg.Transports))
	for name := range s.cfg.Transports {
		transports = append(transports, name)
	}
	sort.Strings(transports)
	var out []cellDefinition
	for _, t := range transports {
		for _, w := range s.cfg.Workloads {
			out = append(out, cellDefinition{Transport: t, Workload: w})
		}
		for i := range s.cfg.Scenarios {
			scenario := s.cfg.Scenarios[i]
			out = append(out, cellDefinition{Transport: t, Scenario: &scenario})
		}
	}
	rng := rand.New(rand.NewSource(s.cfg.Seed))
	rng.Shuffle(len(out), func(i, j int) { out[i], out[j] = out[j], out[i] })
	return out
}

func (s *Sweep) runPoint(ctx context.Context, cell cellDefinition, conns int) (PointRecord, error) {
	transport, testName := cell.Transport, cell.name()
	key := pointKey(transport, testName, s.cfg.Regime, conns)
	comparisonID := comparisonIdentity(s.cfg, cell)
	baseRunDir := filepath.Join(s.cfg.OutputDir, "runs", transport, testName, fmt.Sprintf("c%d", conns))
	cfg := run.RunConfig{
		Transport:          transport,
		ClassMappingSHA256: s.cfg.Transports[transport].ClassMappingSHA256,
		Workload:           cell.Workload,
		Scenario:           cell.Scenario,
		ServerCommand:      s.cfg.Transports[transport].ServerCommand,
		ClientCommand:      s.cfg.Transports[transport].ClientCommand,
		ClientProcs:        s.cfg.Transports[transport].ClientProcs,
		TotalConns:         conns,
		Warmup:             s.cfg.Warmup,
		SteadyWarmup:       s.cfg.SteadyWarmup,
		SteadyMinWarmup:    s.cfg.Transports[transport].SteadyMinWarmup,
		Duration:           s.cfg.Duration,
		Drain:              s.cfg.Drain,
		DeadlineNS:         s.cfg.DeadlineNS,
		StalenessPeriodNS:  s.cfg.StalenessPeriodNS,
		Netem:              s.cfg.Netem,
		NetemGateOff:       false,
		ServerCPUs:         s.cfg.ServerCPUs,
		ClientCPUs:         s.cfg.ClientCPUs,
		SchedIsMeasurand:   s.cfg.Transports[transport].SchedIsMeasurand,
		OutputDir:          baseRunDir,
	}
	cfg, err := cfg.Prepare()
	if err != nil {
		return PointRecord{}, fmt.Errorf("%s: prepare: %w", key, err)
	}
	runIdentity := run.ConfigIdentity(cfg)
	if rec, ok := s.cache[runIdentity]; ok && s.cacheRecordEligible(rec) {
		if rec.ComparisonIdentity != comparisonID ||
			rec.Transport != transport || rec.testName() != testName || rec.Regime != s.cfg.Regime || rec.Conns != conns {
			delete(s.cache, runIdentity)
		} else {
			fmt.Fprintf(os.Stderr, "[sweep] %s cached: ok=%v censored=%v %s\n", key, rec.Judgment.OK, rec.Judgment.Censored, rec.Judgment.Cause)
			return rec, nil
		}
	}
	delete(s.cache, runIdentity)
	if s.attempts == nil {
		s.attempts = map[string]int{}
	}
	s.attempts[runIdentity]++
	attempt := s.attempts[runIdentity]
	runDir := filepath.Join(baseRunDir, runIdentity[:12], fmt.Sprintf("attempt-%03d", attempt))
	cfg.OutputDir = runDir

	start := time.Now()
	fmt.Fprintf(os.Stderr, "[sweep] %s running (duration=%s)...\n", key, cfg.Duration.Duration)
	result, err := run.Run(ctx, cfg)
	if err != nil {
		return PointRecord{}, fmt.Errorf("%s: run: %w", key, err)
	}

	rec := PointRecord{
		Transport: transport,
		Workload:  cell.Workload,
		Scenario: func() string {
			if cell.Scenario != nil {
				return cell.Scenario.Name
			}
			return ""
		}(),
		Regime:  s.cfg.Regime,
		Conns:   conns,
		Verdict: result.Verdict,
		Outcome: result.Outcome,
		Judgment: func() Judgment {
			if cell.Scenario != nil {
				return JudgeScenario(result, *cell.Scenario)
			}
			w, _ := run.LookupWorkload(cell.Workload)
			return Judge(result, w, conns, s.cfg.Netem, s.cfg.StalenessPeriodNS)
		}(),
		DurationS:   time.Since(start).Seconds(),
		RunDir:      runDir,
		RunIdentity: runIdentity,
		AcquisitionID: run.HashValue(struct {
			Campaign, Run string
			Attempt       int
		}{s.campaignIdentity, runIdentity, attempt}),
		Attempt:            attempt,
		CampaignIdentity:   s.campaignIdentity,
		ComparisonIdentity: comparisonID,
	}
	if err := s.appendPointRecord(rec); err != nil {
		return rec, err
	}
	fmt.Fprintf(os.Stderr, "[sweep] %s → ok=%v censored=%v %s\n", key, rec.Judgment.OK, rec.Judgment.Censored, rec.Judgment.Cause)
	return rec, nil
}

func (s *Sweep) appendPointRecord(rec PointRecord) error {
	line, err := json.Marshal(rec)
	if err != nil {
		return err
	}
	if _, err := s.log.Write(append(line, '\n')); err != nil {
		return err
	}
	if rec.RunIdentity != "" && s.cacheRecordEligible(rec) {
		s.cache[rec.RunIdentity] = rec
	} else if rec.RunIdentity != "" {
		delete(s.cache, rec.RunIdentity)
	}
	return nil
}

func measurementInvalidDisposition(rec PointRecord) PointRecord {
	rec.MeasurementInvalid = true
	rec.Outcome = run.OutcomeCensored
	rec.Judgment.Outcome = run.OutcomeCensored
	rec.Judgment.OK = false
	rec.Judgment.Censored = true
	if !strings.HasPrefix(rec.Judgment.Cause, "measurement_invalid:") {
		rec.Judgment.Cause = "measurement_invalid: " + rec.Judgment.Cause
	}
	return rec
}

// Run は全セルの capacity 探索を実行し、capacity.json に結論を書く。
func (s *Sweep) Run(ctx context.Context) ([]CellRecord, error) {
	if s.cfg.Baseline != nil {
		before, err := s.runBaseline(ctx, "before")
		if err != nil {
			return nil, err
		}
		s.block = &BlockBaseline{Before: before}
		if !before.OK {
			s.block.Cause = "baseline before did not pass: " + before.Cause
			if err := s.writeCells(nil); err != nil {
				return nil, err
			}
			return nil, fmt.Errorf("block invalid: %s", s.block.Cause)
		}
	}
	var cells []CellRecord
	for _, cell := range s.cells() {
		if ctx.Err() != nil {
			return cells, ctx.Err()
		}
		var evidenceIDs []string
		eval := func(conns int) (PointOutcome, error) {
			rec, err := s.runPoint(ctx, cell, conns)
			if err != nil {
				return PointOutcome{}, err
			}
			if rec.Outcome == run.OutcomeUnsupported || rec.Outcome == run.OutcomeInconclusive {
				if rec.AcquisitionID != "" {
					evidenceIDs = append(evidenceIDs, rec.AcquisitionID)
				}
				return PointOutcome{Outcome: rec.Outcome, Cause: strings.Join(rec.OutcomeReasons(), "; ")}, nil
			}
			// 測定不成立(インフラ故障・環境不成立)は break でも censored でも
			// ないので1回だけ再測する(再生計画 D3)。再測も不成立なら
			// censored 扱いで打ち切り、原因に invalid を残す(誤って「server の
			// break」として capacity に載せない)
			if pointMeasurementInvalid(rec) {
				fmt.Fprintf(os.Stderr, "[sweep] %s invalid — retrying once: %s\n",
					pointKey(cell.Transport, cell.name(), s.cfg.Regime, conns), rec.Judgment.Cause)
				delete(s.cache, rec.RunIdentity)
				rec, err = s.runPoint(ctx, cell, conns)
				if err != nil {
					return PointOutcome{}, err
				}
				if pointMeasurementInvalid(rec) {
					rec = measurementInvalidDisposition(rec)
					if err := s.appendPointRecord(rec); err != nil {
						return PointOutcome{}, err
					}
					if rec.AcquisitionID != "" {
						evidenceIDs = append(evidenceIDs, rec.AcquisitionID)
					}
					return PointOutcome{Censored: rec.Judgment.Censored, MeasurementInvalid: true, Cause: rec.Judgment.Cause}, nil
				}
			}
			if rec.AcquisitionID != "" {
				evidenceIDs = append(evidenceIDs, rec.AcquisitionID)
			}
			return PointOutcome{OK: rec.Judgment.OK, Censored: rec.Judgment.Censored, MeasurementInvalid: rec.MeasurementInvalid, Cause: rec.Judgment.Cause}, nil
		}
		cap, err := FindCapacity(eval, s.cfg.Conns.Min, s.cfg.Conns.Max)
		if err != nil {
			return cells, fmt.Errorf("cell %s/%s: %w", cell.Transport, cell.name(), err)
		}
		rec := CellRecord{
			Transport: cell.Transport, Workload: cell.Workload, Regime: s.cfg.Regime,
			CampaignIdentity: s.campaignIdentity, ComparisonIdentity: comparisonIdentity(s.cfg, cell),
			EvidenceIDs: dedupeSorted(evidenceIDs), CellCapacity: cap,
		}
		if cell.Scenario != nil {
			rec.Scenario = cell.Scenario.Name
		}
		cells = append(cells, rec)
		fmt.Fprintf(os.Stderr, "[sweep] CELL %s/%s/%s capacity=%d censored=%v range_limited=%v break=%d cause=%s\n",
			cell.Transport, cell.name(), s.cfg.Regime, cap.Capacity, cap.Censored, cap.RangeLimited, cap.BreakConns, cap.BreakCause)
		if err := s.writeCells(cells); err != nil {
			return cells, err
		}
	}
	if s.cfg.Baseline != nil {
		after, err := s.runBaseline(ctx, "after")
		if err != nil {
			return cells, err
		}
		s.block.After = &after
		s.block.DriftOK, s.block.Cause = evaluateDrift(s.block.Before, after, s.cfg.Baseline.Drift)
		if !s.block.DriftOK {
			for i := range cells {
				cells[i].BlockInvalid = true
				cells[i].BlockInvalidCause = s.block.Cause
			}
		}
		if err := s.writeCells(cells); err != nil {
			return cells, err
		}
		if !s.block.DriftOK {
			return cells, fmt.Errorf("block invalid: %s", s.block.Cause)
		}
	}
	return cells, nil
}

func pointMeasurementInvalid(rec PointRecord) bool {
	if rec.Judgment.Censored {
		return false
	}
	return rec.Outcome == run.OutcomeInvalid ||
		(rec.Outcome == "" && strings.HasPrefix(rec.Judgment.Cause, "invalid:"))
}

func (r PointRecord) OutcomeReasons() []string {
	if r.Judgment.Cause == "" {
		return nil
	}
	return []string{r.Judgment.Cause}
}

func dedupeSorted(values []string) []string {
	seen := map[string]bool{}
	for _, value := range values {
		if value != "" {
			seen[value] = true
		}
	}
	out := make([]string, 0, len(seen))
	for value := range seen {
		out = append(out, value)
	}
	sort.Strings(out)
	return out
}

func (s *Sweep) writeCells(cells []CellRecord) error {
	data, err := json.MarshalIndent(struct {
		Seed          int64          `json:"seed"`
		BlockBaseline *BlockBaseline `json:"block_baseline,omitempty"`
		Cells         []CellRecord   `json:"cells"`
	}{s.cfg.Seed, s.block, cells}, "", " ")
	if err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(s.cfg.OutputDir, "capacity.json"), append(data, '\n'), 0o644)
}
