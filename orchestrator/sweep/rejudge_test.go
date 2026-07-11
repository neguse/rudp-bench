package sweep

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/neguse/rudp-bench/orchestrator/control"
	"github.com/neguse/rudp-bench/orchestrator/netops"
	"github.com/neguse/rudp-bench/orchestrator/run"
)

func pt(conns int, ok, censored bool, cause string) PointRecord {
	return PointRecord{Transport: "t", Workload: "w", Regime: "r", Conns: conns,
		Judgment: Judgment{OK: ok, Censored: censored, Cause: cause}}
}

func TestRejudgeAnalysisIdentityIncludesNormalizedOverrides(t *testing.T) {
	base := rejudgeAnalysisIdentity("campaign", nil, "evidence")
	if got := rejudgeAnalysisIdentity("campaign", map[string]bool{"websocket": false}, "evidence"); got != base {
		t.Fatalf("disabled override changed identity: %s != %s", got, base)
	}
	first := rejudgeAnalysisIdentity("campaign", map[string]bool{"websocket": true, "magiconion": true}, "evidence")
	second := rejudgeAnalysisIdentity("campaign", map[string]bool{"magiconion": true, "websocket": true}, "evidence")
	if first != second {
		t.Fatalf("map order changed analysis identity: %s != %s", first, second)
	}
	if first == base {
		t.Fatal("enabled sched-measurand override did not change analysis identity")
	}
	if rejudgeAnalysisIdentity("campaign", nil, "changed") == base {
		t.Fatal("source evidence change did not change analysis identity")
	}
}

func TestRejudgeSourceEvidenceSeparatesAttempts(t *testing.T) {
	records := make([]PointRecord, 2)
	for i := range records {
		runDir := filepath.Join(t.TempDir(), "attempt")
		if err := os.MkdirAll(runDir, 0o755); err != nil {
			t.Fatal(err)
		}
		content := []byte(fmt.Sprintf(`{"attempt":%d}`, i+1))
		if err := os.WriteFile(filepath.Join(runDir, "result.json"), content, 0o644); err != nil {
			t.Fatal(err)
		}
		records[i] = PointRecord{
			CampaignIdentity: "campaign", RunIdentity: "same-run", AcquisitionID: fmt.Sprintf("acq-%d", i+1),
			Attempt: i + 1, RunDir: runDir,
		}
	}
	_, results, err := rejudgeSourceEvidence(records, "campaign")
	if err != nil {
		t.Fatal(err)
	}
	if len(results) != 2 || string(results[0]) == string(results[1]) {
		t.Fatalf("attempt evidence collided: results=%v", results)
	}
}

func TestDeriveCellCensoredBeforeFail(t *testing.T) {
	cell := deriveCell([]PointRecord{
		pt(32, true, false, ""),
		pt(64, true, false, ""),
		pt(66, false, true, "farm_limited: pacing stall"),
		pt(128, false, false, "staleness"),
	})
	if !cell.Censored || cell.Capacity != 64 || cell.BreakConns != 0 {
		t.Fatalf("cell = %+v", cell)
	}
}

func TestDeriveCellPlainBreak(t *testing.T) {
	cell := deriveCell([]PointRecord{
		pt(1, true, false, ""),
		pt(64, true, false, ""),
		pt(96, false, false, "delivery_lt"),
		pt(80, true, false, ""),
	})
	if cell.Censored || cell.Capacity != 80 || cell.BreakConns != 96 || cell.BreakCause != "delivery_lt" {
		t.Fatalf("cell = %+v", cell)
	}
}

func TestDeriveCellAllOK(t *testing.T) {
	cell := deriveCell([]PointRecord{pt(1, true, false, ""), pt(1024, true, false, "")})
	if !cell.RangeLimited || cell.Capacity != 1024 {
		t.Fatalf("cell = %+v", cell)
	}
}

func TestDeriveCellPreservesTerminalOutcome(t *testing.T) {
	point := pt(4, false, false, "scenario not advertised")
	point.Outcome = run.OutcomeUnsupported
	point.Judgment.Outcome = run.OutcomeUnsupported
	cell := deriveCell([]PointRecord{point})
	if cell.Outcome != run.OutcomeUnsupported || cell.BreakCause != "scenario not advertised" ||
		cell.Capacity != 0 || cell.BreakConns != 0 || cell.RangeLimited || cell.Censored {
		t.Fatalf("terminal cell = %+v", cell)
	}
}

func TestRejudgeRejectsPersistedMetricsAggregateMismatch(t *testing.T) {
	dir := t.TempDir()
	runDir := filepath.Join(dir, "run")
	if err := os.MkdirAll(runDir, 0o755); err != nil {
		t.Fatal(err)
	}
	empty := rejudgeHistogram(nil)
	one := rejudgeHistogram(map[int]uint64{4: 1})
	result := run.Result{
		Version: 2, Verdict: run.VerdictValid, Outcome: run.OutcomePass,
		Config: run.RunConfig{Workload: "reliable_echo", TotalConns: 1},
		Metrics: &run.MergedMetrics{
			Version: 2,
			Classes: map[string]run.ClassAggregate{
				run.ClassLossTolerant: {LatencySchedNS: empty, LatencySendNS: empty, UpdateGapNS: empty},
				run.ClassMustDeliver: {
					ClassCounts:    run.ClassCounts{DeliveredUnique: 1, DeadlineHit: 0},
					LatencySchedNS: one, LatencySendNS: one, UpdateGapNS: empty,
				},
			},
			StalenessNS: empty,
			Traffic: []run.TrafficAggregate{{
				TrafficID: 1, Direction: run.DirectionRoomRelay, Class: run.ClassMustDeliver,
				ClassCounts:    run.ClassCounts{DeliveredUnique: 1, DeadlineHit: 1},
				LatencySchedNS: one, LatencySendNS: one, UpdateGapNS: empty, StalenessNS: empty,
			}},
		},
	}
	resultData, err := json.Marshal(result)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(runDir, "result.json"), resultData, 0o644); err != nil {
		t.Fatal(err)
	}
	record := PointRecord{
		Transport: "raw", Workload: "reliable_echo", Regime: "wired", Conns: 1,
		RunDir: runDir, RunIdentity: "run", CampaignIdentity: "campaign",
		ComparisonIdentity: "comparison", Verdict: run.VerdictValid, Outcome: run.OutcomePass,
		Judgment: Judgment{Outcome: run.OutcomePass, OK: true},
	}
	line, err := json.Marshal(record)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "results.jsonl"), append(line, '\n'), 0o644); err != nil {
		t.Fatal(err)
	}
	capacity := `{"seed":1,"cells":[{"transport":"raw","workload":"reliable_echo","regime":"wired","campaign_identity":"campaign","comparison_identity":"comparison","capacity":1,"evaluated_points":1}]}`
	if err := os.WriteFile(filepath.Join(dir, "capacity.json"), []byte(capacity), 0o644); err != nil {
		t.Fatal(err)
	}

	if err := Rejudge(dir, nil); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(filepath.Join(dir, "results.jsonl"))
	if err != nil {
		t.Fatal(err)
	}
	var got PointRecord
	if err := json.Unmarshal(data, &got); err != nil {
		t.Fatal(err)
	}
	if got.Outcome != run.OutcomeCensored || got.Verdict != run.VerdictInvalid || got.Judgment.Outcome != run.OutcomeCensored ||
		!got.MeasurementInvalid || !got.Judgment.Censored || got.Judgment.OK ||
		!strings.Contains(got.Judgment.Cause, "deadline_hit=0, want traffic sum=1") {
		t.Fatalf("rejudged mismatch = %+v", got)
	}
	capacityData, err := os.ReadFile(filepath.Join(dir, "capacity.json"))
	if err != nil {
		t.Fatal(err)
	}
	var capacityDoc struct {
		Cells []CellRecord `json:"cells"`
	}
	if err := json.Unmarshal(capacityData, &capacityDoc); err != nil {
		t.Fatal(err)
	}
	if len(capacityDoc.Cells) != 1 || !capacityDoc.Cells[0].Censored || !capacityDoc.Cells[0].MeasurementInvalid || capacityDoc.Cells[0].BreakConns != 0 {
		t.Fatalf("invalid measurement became a numeric break: %+v", capacityDoc.Cells)
	}
}

func TestRejudgeInvalidatesLossRunWithoutInWindowQdiscEvidence(t *testing.T) {
	dir := t.TempDir()
	runDir := filepath.Join(dir, "run")
	if err := os.MkdirAll(runDir, 0o755); err != nil {
		t.Fatal(err)
	}
	schedule := control.ScheduleMessage{Type: control.TypeSchedule, StartAtNS: 100, StopAtNS: 200, DrainUntilNS: 220}
	result := run.Result{
		Version: 2, Verdict: run.VerdictValid, Outcome: run.OutcomePass,
		Config: run.RunConfig{
			Workload: "reliable_echo", TotalConns: 1,
			Netem: &run.NetemRegime{ClientEgress: netops.Netem{LossPercent: 1}},
		},
		Control: &control.Result{Schedule: schedule},
	}
	resultData, err := json.Marshal(result)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(runDir, "result.json"), resultData, 0o644); err != nil {
		t.Fatal(err)
	}
	record := PointRecord{
		Transport: "raw", Workload: "reliable_echo", Regime: "loss", Conns: 1,
		RunDir: runDir, RunIdentity: "run", CampaignIdentity: "campaign",
		ComparisonIdentity: "comparison", Verdict: run.VerdictValid, Outcome: run.OutcomePass,
		Judgment: Judgment{Outcome: run.OutcomePass, OK: true},
	}
	line, err := json.Marshal(record)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "results.jsonl"), append(line, '\n'), 0o644); err != nil {
		t.Fatal(err)
	}
	capacity := `{"seed":1,"cells":[{"transport":"raw","workload":"reliable_echo","regime":"loss","campaign_identity":"campaign","comparison_identity":"comparison","capacity":1,"evaluated_points":1}]}`
	if err := os.WriteFile(filepath.Join(dir, "capacity.json"), []byte(capacity), 0o644); err != nil {
		t.Fatal(err)
	}

	if err := Rejudge(dir, nil); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(filepath.Join(dir, "results.jsonl"))
	if err != nil {
		t.Fatal(err)
	}
	var got PointRecord
	if err := json.Unmarshal(data, &got); err != nil {
		t.Fatal(err)
	}
	if got.Outcome != run.OutcomeCensored || !got.MeasurementInvalid || !got.Judgment.Censored ||
		!strings.Contains(got.Judgment.Cause, "netem loss evidence: missing") {
		t.Fatalf("loss evidence omission was not measurement_invalid: %+v", got)
	}
}

func rejudgeHistogram(counts map[int]uint64) run.Histogram {
	bins := make([]uint64, 448)
	var count uint64
	for index, value := range counts {
		bins[index] = value
		count += value
	}
	return run.Histogram{Scheme: "log2x16", MinNS: 1_000, MaxNS: 100_000_000_000, Count: count, Bins: bins}
}

func TestReadCapacityMetadata(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "capacity.json")
	data := []byte(`{"seed":42,"cells":[
{"transport":"a","scenario":"s","campaign_identity":"campaign","regime":"r"},
{"transport":"b","scenario":"s","campaign_identity":"campaign","regime":"r"}
]}`)
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}
	seed, campaign, err := readCapacityMetadata(path)
	if err != nil {
		t.Fatal(err)
	}
	if seed != 42 || campaign != "campaign" {
		t.Fatalf("metadata = seed %d campaign %q", seed, campaign)
	}

	mixed := []byte(`{"seed":42,"cells":[
{"transport":"a","scenario":"s","campaign_identity":"one","regime":"r"},
{"transport":"b","scenario":"s","campaign_identity":"two","regime":"r"}
]}`)
	if err := os.WriteFile(path, mixed, 0o644); err != nil {
		t.Fatal(err)
	}
	if _, _, err := readCapacityMetadata(path); err == nil {
		t.Fatal("mixed campaigns unexpectedly accepted")
	}
}

func TestRejudgePreservesMeasurementInvalidDisposition(t *testing.T) {
	dir := t.TempDir()
	record := measurementInvalidDisposition(PointRecord{
		Transport: "raw", Workload: "echo", Regime: "wired", Conns: 1,
		RunDir: filepath.Join(dir, "missing-run"), RunIdentity: "run",
		CampaignIdentity: "campaign", ComparisonIdentity: "comparison",
		Judgment: Judgment{Cause: "invalid: control failed"},
	})
	line, err := json.Marshal(record)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "results.jsonl"), append(line, '\n'), 0o644); err != nil {
		t.Fatal(err)
	}
	capacity := `{"seed":1,"cells":[{"transport":"raw","workload":"echo","regime":"wired","campaign_identity":"campaign","comparison_identity":"comparison","censored":true,"break_cause":"measurement_invalid: invalid: control failed","evaluated_points":1}]}`
	if err := os.WriteFile(filepath.Join(dir, "capacity.json"), []byte(capacity), 0o644); err != nil {
		t.Fatal(err)
	}

	if err := Rejudge(dir, nil); err != nil {
		t.Fatal(err)
	}

	var got struct {
		Cells []CellRecord `json:"cells"`
	}
	data, err := os.ReadFile(filepath.Join(dir, "capacity.json"))
	if err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(data, &got); err != nil {
		t.Fatal(err)
	}
	if len(got.Cells) != 1 || !got.Cells[0].Censored || got.Cells[0].BreakConns != 0 ||
		got.Cells[0].ComparisonIdentity == "comparison" || got.Cells[0].AnalysisIdentity == "" ||
		!strings.HasPrefix(got.Cells[0].BreakCause, "measurement_invalid:") {
		t.Fatalf("rejudged capacity = %+v", got.Cells)
	}

	data, err = os.ReadFile(filepath.Join(dir, "results.jsonl"))
	if err != nil {
		t.Fatal(err)
	}
	var persisted PointRecord
	if err := json.Unmarshal(data, &persisted); err != nil {
		t.Fatal(err)
	}
	if !persisted.MeasurementInvalid || !persisted.Judgment.Censored || !persisted.Rejudged ||
		persisted.AnalysisIdentity == "" || persisted.SourceJudgment == nil {
		t.Fatalf("persisted disposition = %+v", persisted)
	}
	s := &Sweep{
		cfg: Config{OutputDir: dir}, cache: map[string]PointRecord{},
		campaignIdentity: "campaign",
	}
	if err := s.loadResume(); err != nil {
		t.Fatal(err)
	}
	if got, ok := s.cache[record.RunIdentity]; ok {
		t.Fatalf("rejudged invalid record became a resume source: %+v", got)
	}
}

func TestRejudgeRecordsAreNotResumeCacheSources(t *testing.T) {
	dir := t.TempDir()
	runDir := filepath.Join(dir, "run")
	if err := os.MkdirAll(runDir, 0o755); err != nil {
		t.Fatal(err)
	}
	result := run.Result{
		Verdict:        run.VerdictInvalid,
		InvalidReasons: []string{"control failed"},
		Config:         run.RunConfig{Workload: "echo", TotalConns: 1},
	}
	resultData, err := json.Marshal(result)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(runDir, "result.json"), resultData, 0o644); err != nil {
		t.Fatal(err)
	}
	record := PointRecord{
		Transport: "raw", Workload: "echo", Regime: "wired", Conns: 1,
		RunDir: runDir, RunIdentity: "run", CampaignIdentity: "campaign",
		ComparisonIdentity: "comparison", Judgment: Judgment{OK: true},
	}
	line, err := json.Marshal(record)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "results.jsonl"), append(line, '\n'), 0o644); err != nil {
		t.Fatal(err)
	}
	capacity := `{"seed":1,"cells":[{"transport":"raw","workload":"echo","regime":"wired","campaign_identity":"campaign","comparison_identity":"comparison","capacity":1,"evaluated_points":1}]}`
	if err := os.WriteFile(filepath.Join(dir, "capacity.json"), []byte(capacity), 0o644); err != nil {
		t.Fatal(err)
	}

	if err := Rejudge(dir, nil); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(filepath.Join(dir, "results.jsonl"))
	if err != nil {
		t.Fatal(err)
	}
	var persisted PointRecord
	if err := json.Unmarshal(data, &persisted); err != nil {
		t.Fatal(err)
	}
	if !persisted.Rejudged {
		t.Fatalf("rejudged marker missing: %+v", persisted)
	}
	firstAnalysis := persisted.AnalysisIdentity
	firstComparison := persisted.ComparisonIdentity
	firstSourceJudgment := run.HashValue(persisted.SourceJudgment)
	firstSourceComparison := persisted.SourceComparisonIdentity
	if err := Rejudge(dir, nil); err != nil {
		t.Fatal(err)
	}
	data, err = os.ReadFile(filepath.Join(dir, "results.jsonl"))
	if err != nil {
		t.Fatal(err)
	}
	var repeated PointRecord
	if err := json.Unmarshal(data, &repeated); err != nil {
		t.Fatal(err)
	}
	if repeated.AnalysisIdentity != firstAnalysis || repeated.ComparisonIdentity != firstComparison ||
		repeated.SourceComparisonIdentity != firstSourceComparison || run.HashValue(repeated.SourceJudgment) != firstSourceJudgment {
		t.Fatalf("same rejudge was not idempotent: first=%+v repeated=%+v", persisted, repeated)
	}
	sourcePath := filepath.Join(runDir, "result.json")
	sourceData, err := os.ReadFile(sourcePath)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(sourcePath, append(sourceData, '\n'), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := Rejudge(dir, nil); err != nil {
		t.Fatal(err)
	}
	data, err = os.ReadFile(filepath.Join(dir, "results.jsonl"))
	if err != nil {
		t.Fatal(err)
	}
	var changedSource PointRecord
	if err := json.Unmarshal(data, &changedSource); err != nil {
		t.Fatal(err)
	}
	if changedSource.AnalysisIdentity == firstAnalysis || changedSource.SourceResultSHA256 == persisted.SourceResultSHA256 {
		t.Fatalf("changed source result was skipped: first=%+v changed=%+v", persisted, changedSource)
	}

	s := &Sweep{
		cfg: Config{OutputDir: dir}, cache: map[string]PointRecord{},
		campaignIdentity: "campaign",
	}
	if err := s.loadResume(); err != nil {
		t.Fatal(err)
	}
	if _, ok := s.cache[record.RunIdentity]; ok {
		t.Fatal("rejudged record was loaded into resume cache")
	}
}
