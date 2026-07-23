package sweep

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"

	"github.com/neguse/rudp-bench/orchestrator/run"
)

// Rejudge は既存 sweep 出力の判定だけを run 成果物(result.json)から
// 再計算する。再測定はしない — gate/floor の意味論を更新したときに、
// 測定済みデータへ新しい判定を適用するために使う。
// capacity.json は探索済みの点集合から再導出する(単調性を仮定)。
func Rejudge(dir string, schedMeasurand map[string]bool) error {
	seed, activeCampaign, err := readCapacityMetadata(filepath.Join(dir, "capacity.json"))
	if err != nil {
		return err
	}
	resultsPath := filepath.Join(dir, "results.jsonl")
	f, err := os.Open(resultsPath)
	if err != nil {
		return err
	}
	var records []PointRecord
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 1<<20), 1<<20)
	lineNo := 0
	for sc.Scan() {
		lineNo++
		var rec PointRecord
		if err := json.Unmarshal(sc.Bytes(), &rec); err != nil {
			return fmt.Errorf("%s line %d: %w", resultsPath, lineNo, err)
		}
		records = append(records, rec)
	}
	if err := sc.Err(); err != nil {
		f.Close()
		return err
	}
	f.Close()
	sourceEvidenceIdentity, sourceResults, err := rejudgeSourceEvidence(records, activeCampaign)
	if err != nil {
		return err
	}
	analysisIdentity := rejudgeAnalysisIdentity(activeCampaign, schedMeasurand, sourceEvidenceIdentity)

	for i := range records {
		rec := &records[i]
		if rec.CampaignIdentity != activeCampaign {
			continue
		}
		if rec.Rejudged && rec.AnalysisIdentity == analysisIdentity {
			continue
		}
		preserveSourceProvenance(rec)
		if rec.SourceMeasurementInvalid != nil && *rec.SourceMeasurementInvalid {
			*rec = measurementInvalidDisposition(*rec)
			markRejudgedProvenance(rec, analysisIdentity, "")
			continue
		}
		rec.MeasurementInvalid = false
		data := sourceResults[i]
		if len(data) == 0 {
			return fmt.Errorf("%s: source result is missing from rejudge evidence", rec.RunDir)
		}
		var res run.Result
		if err := json.Unmarshal(data, &res); err != nil {
			return fmt.Errorf("%s/result.json: %w", rec.RunDir, err)
		}
		// 過去 run の result.json にはフラグが無いため、transport 単位で上書きする
		if schedMeasurand[rec.Transport] {
			res.Config.SchedIsMeasurand = true
		}
		var contractInvalid []string
		treatmentInvalid, treatmentUnsupported := run.ValidateScenarioTreatmentContract(res.Treatment, res.Config)
		for _, reason := range treatmentInvalid {
			contractInvalid = append(contractInvalid, "treatment contract: "+reason)
		}
		if len(contractInvalid) == 0 && len(treatmentUnsupported) > 0 {
			res.Outcome = run.OutcomeUnsupported
			res.OutcomeReasons = append([]string(nil), treatmentUnsupported...)
			res.Verdict = run.VerdictInvalid
			res.InvalidReasons = nil
		} else {
			if err := run.ValidateMergedMetricsConsistency(res.Metrics); err != nil {
				contractInvalid = append(contractInvalid, "metrics contract: "+err.Error())
			}
			var lossEvidence *run.NetemLossEvidence
			if res.Netem != nil {
				lossEvidence = res.Netem.LossEvidence
			}
			contractInvalid = append(contractInvalid, run.ValidateNetemLossEvidence(&res.Config, res.Control, lossEvidence)...)
		}
		if len(contractInvalid) > 0 {
			res.Verdict = run.VerdictInvalid
			res.InvalidReasons = append(res.InvalidReasons, contractInvalid...)
			res.Outcome = run.OutcomeInvalid
			res.OutcomeReasons = append([]string(nil), contractInvalid...)
		}
		old := rec.Judgment
		rec.SourceResultSHA256 = run.HashBytes(data)
		if res.Config.Scenario != nil {
			rec.Judgment = JudgeScenario(&res, *res.Config.Scenario)
		} else {
			w, ok := run.LookupWorkload(res.Config.Workload)
			if !ok {
				return fmt.Errorf("%s: unknown workload %q", rec.RunDir, res.Config.Workload)
			}
			rec.Judgment = Judge(&res, w, res.Config.TotalConns, res.Config.Netem, res.Config.StalenessPeriodNS)
		}
		rec.Outcome = rec.Judgment.Outcome
		rec.Verdict = res.Verdict
		if rec.Outcome == run.OutcomeInvalid {
			*rec = measurementInvalidDisposition(*rec)
		}
		markRejudgedProvenance(rec, analysisIdentity, run.HashBytes(data))
		if old.OK != rec.Judgment.OK || old.Censored != rec.Judgment.Censored {
			fmt.Fprintf(os.Stderr, "[rejudge] %s|%s c%d: ok=%v→%v censored=%v→%v %s\n",
				rec.Transport, rec.testName(), rec.Conns, old.OK, rec.Judgment.OK, old.Censored, rec.Judgment.Censored, rec.Judgment.Cause)
		}
	}

	// results.jsonl を書き直す
	tmp := resultsPath + ".tmp"
	out, err := os.Create(tmp)
	if err != nil {
		return err
	}
	for _, rec := range records {
		line, err := json.Marshal(rec)
		if err != nil {
			out.Close()
			return err
		}
		if _, err := out.Write(append(line, '\n')); err != nil {
			out.Close()
			return err
		}
	}
	if err := out.Close(); err != nil {
		return err
	}
	if err := os.Rename(tmp, resultsPath); err != nil {
		return err
	}

	// capacity.json をセルごとに再導出
	latest := map[string]PointRecord{}
	for _, rec := range records {
		if rec.CampaignIdentity != activeCampaign {
			continue
		}
		key := rec.RunIdentity
		if key == "" {
			key = pointKey(rec.Transport, rec.testName(), rec.Regime, rec.Conns)
		}
		latest[key] = rec
	}
	type cellKey struct {
		campaign  string
		transport string
		testName  string
		regime    string
	}
	byCell := map[cellKey][]PointRecord{}
	comparisonByCell := map[cellKey]string{}
	for _, rec := range latest {
		key := cellKey{rec.CampaignIdentity, rec.Transport, rec.testName(), rec.Regime}
		if comparison, ok := comparisonByCell[key]; ok && comparison != rec.ComparisonIdentity {
			return fmt.Errorf("cell %s/%s/%s mixes comparison identities", rec.Transport, rec.testName(), rec.Regime)
		}
		comparisonByCell[key] = rec.ComparisonIdentity
		byCell[key] = append(byCell[key], rec)
	}
	keys := make([]cellKey, 0, len(byCell))
	for k := range byCell {
		keys = append(keys, k)
	}
	sort.Slice(keys, func(i, j int) bool {
		if keys[i].transport != keys[j].transport {
			return keys[i].transport < keys[j].transport
		}
		if keys[i].testName != keys[j].testName {
			return keys[i].testName < keys[j].testName
		}
		return keys[i].regime < keys[j].regime
	})

	var cells []CellRecord
	for _, k := range keys {
		points := byCell[k]
		rec := CellRecord{
			Transport:          points[0].Transport,
			Workload:           points[0].Workload,
			Scenario:           points[0].Scenario,
			Preset:             points[0].Preset,
			PresetHash:         points[0].PresetHash,
			CampaignIdentity:   points[0].CampaignIdentity,
			ComparisonIdentity: points[0].ComparisonIdentity,
			Regime:             points[0].Regime,
			AnalysisIdentity:   analysisIdentity,
			EvidenceIDs:        pointEvidenceIDs(points),
			CellCapacity:       deriveCell(points),
		}
		cells = append(cells, rec)
	}
	data, err := json.MarshalIndent(struct {
		Seed  int64        `json:"seed"`
		Cells []CellRecord `json:"cells"`
	}{seed, cells}, "", " ")
	if err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(dir, "capacity.json"), append(data, '\n'), 0o644)
}

func rejudgeAnalysisIdentity(activeCampaign string, schedMeasurand map[string]bool, sourceEvidenceIdentity string) string {
	normalized := map[string]bool{}
	for transport, enabled := range schedMeasurand {
		if enabled {
			normalized[transport] = true
		}
	}
	return run.HashValue(struct {
		Operation          string          `json:"operation"`
		SourceCampaign     string          `json:"source_campaign"`
		OrchestratorSHA256 string          `json:"orchestrator_sha256"`
		SchedMeasurand     map[string]bool `json:"sched_measurand"`
		SourceEvidence     string          `json:"source_evidence_sha256"`
	}{"rejudge-v4", activeCampaign, run.OrchestratorFingerprint(), normalized, sourceEvidenceIdentity})
}

func rejudgeSourceEvidence(records []PointRecord, activeCampaign string) (string, map[int][]byte, error) {
	type evidence struct {
		Key             string `json:"key"`
		AcquisitionID   string `json:"acquisition_id,omitempty"`
		ResultSHA256    string `json:"result_sha256,omitempty"`
		OriginalInvalid bool   `json:"original_measurement_invalid,omitempty"`
		JudgmentSHA256  string `json:"judgment_sha256,omitempty"`
	}
	var items []evidence
	results := map[int][]byte{}
	for index, rec := range records {
		if rec.CampaignIdentity != activeCampaign {
			continue
		}
		key := rejudgeEvidenceKey(rec, index)
		originalInvalid := rec.MeasurementInvalid && rec.SourceResultSHA256 == ""
		if rec.SourceMeasurementInvalid != nil {
			originalInvalid = *rec.SourceMeasurementInvalid
		}
		item := evidence{Key: key, AcquisitionID: rec.AcquisitionID, OriginalInvalid: originalInvalid}
		if originalInvalid {
			judgment := rec.Judgment
			if rec.SourceJudgment != nil {
				judgment = *rec.SourceJudgment
			}
			item.JudgmentSHA256 = run.HashValue(judgment)
		} else {
			data, err := os.ReadFile(filepath.Join(rec.RunDir, "result.json"))
			if err != nil {
				return "", nil, fmt.Errorf("%s: %w", rec.RunDir, err)
			}
			results[index] = data
			item.ResultSHA256 = run.HashBytes(data)
		}
		items = append(items, item)
	}
	sort.Slice(items, func(i, j int) bool { return items[i].Key < items[j].Key })
	return run.HashValue(items), results, nil
}

func rejudgeEvidenceKey(rec PointRecord, index int) string {
	return fmt.Sprintf("record:%08d|acquisition:%s|run:%s|attempt:%d|dir:%s",
		index, rec.AcquisitionID, rec.RunIdentity, rec.Attempt, rec.RunDir)
}

func markRejudgedProvenance(rec *PointRecord, analysisIdentity, sourceResultSHA string) {
	rec.Rejudged = true
	rec.AnalysisIdentity = analysisIdentity
	if sourceResultSHA != "" {
		rec.SourceResultSHA256 = sourceResultSHA
	}
	rec.ComparisonIdentity = run.HashValue(struct {
		SourceComparison string `json:"source_comparison"`
		Analysis         string `json:"analysis_identity"`
	}{rec.SourceComparisonIdentity, analysisIdentity})
}

func preserveSourceProvenance(rec *PointRecord) {
	if rec.SourceJudgment == nil {
		judgment := rec.Judgment
		rec.SourceJudgment = &judgment
	}
	if rec.SourceComparisonIdentity == "" {
		rec.SourceComparisonIdentity = rec.ComparisonIdentity
	}
	if rec.SourceMeasurementInvalid == nil {
		value := rec.MeasurementInvalid
		rec.SourceMeasurementInvalid = &value
	}
}

func pointEvidenceIDs(points []PointRecord) []string {
	var ids []string
	for _, point := range points {
		if point.AcquisitionID != "" {
			ids = append(ids, point.AcquisitionID)
		}
	}
	return dedupeSorted(ids)
}

func readCapacityMetadata(capacityPath string) (int64, string, error) {
	var doc struct {
		Seed  int64        `json:"seed"`
		Cells []CellRecord `json:"cells"`
	}
	data, err := os.ReadFile(capacityPath)
	if err != nil {
		return 0, "", err
	}
	if err := json.Unmarshal(data, &doc); err != nil {
		return 0, "", fmt.Errorf("%s: %w", capacityPath, err)
	}
	if len(doc.Cells) == 0 {
		return 0, "", fmt.Errorf("%s has no cells", capacityPath)
	}
	campaign := doc.Cells[0].CampaignIdentity
	for _, cell := range doc.Cells[1:] {
		if cell.CampaignIdentity != campaign {
			return 0, "", fmt.Errorf("%s mixes campaign identities", capacityPath)
		}
	}
	return doc.Seed, campaign, nil
}

// deriveCell は探索済み点集合から capacity 結論を再導出する(単調性仮定)。
// 探索順は再現しないため、bisection が本来引いた境界の近似になる。
func deriveCell(points []PointRecord) CellCapacity {
	sort.Slice(points, func(i, j int) bool { return points[i].Conns < points[j].Conns })
	cell := CellCapacity{Evaluated: len(points)}
	for _, p := range points {
		outcome := p.Outcome
		if outcome == "" {
			outcome = p.Judgment.Outcome
		}
		if isTerminalPointOutcome(outcome) {
			cell.Outcome = outcome
			cell.BreakCause = p.Judgment.Cause
			return cell
		}
	}
	for _, p := range points {
		if p.Judgment.OK {
			cell.Capacity = p.Conns
		}
	}
	firstFail, firstCensor := 0, 0
	firstCensorMeasurementInvalid := false
	var failCause, censorCause string
	for _, p := range points {
		if p.Conns <= cell.Capacity {
			continue
		}
		if p.Judgment.Censored {
			if firstCensor == 0 {
				firstCensor = p.Conns
				censorCause = p.Judgment.Cause
				firstCensorMeasurementInvalid = p.MeasurementInvalid
			}
			continue
		}
		if !p.Judgment.OK && firstFail == 0 {
			firstFail = p.Conns
			failCause = p.Judgment.Cause
		}
	}
	switch {
	case firstCensor > 0 && (firstFail == 0 || firstCensor < firstFail):
		cell.Censored = true
		cell.MeasurementInvalid = firstCensorMeasurementInvalid
		cell.BreakCause = censorCause
	case firstFail > 0:
		cell.BreakConns = firstFail
		cell.BelowRange = cell.Capacity == 0 && firstFail > 1
		cell.BreakCause = failCause
	default:
		cell.RangeLimited = true
	}
	return cell
}
