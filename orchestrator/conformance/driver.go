package conformance

import (
	"context"
	"fmt"
	"path/filepath"
	"reflect"
	"sort"
	"strings"

	"github.com/neguse/rudp-bench/orchestrator/run"
)

const (
	SessionReportVersion = 1
	AttemptRecordVersion = 1
)

type AttemptStatus string

const (
	AttemptAccepted       AttemptStatus = "accepted"
	AttemptInvalid        AttemptStatus = "invalid"
	AttemptSUTFailure     AttemptStatus = "sut_failure"
	AttemptRunnerError    AttemptStatus = "runner_error"
	AttemptAbandoned      AttemptStatus = "abandoned"
	AttemptDependencySkip AttemptStatus = "dependency_skip"
)

type AttemptPlanReceipt struct {
	Version           int         `json:"version"`
	SessionIdentity   string      `json:"session_identity"`
	SessionPlanSHA256 string      `json:"session_plan_sha256"`
	Attempt           AttemptPlan `json:"attempt"`
}

type ResultArtifactReceipt struct {
	Version      int    `json:"version"`
	SlotIdentity string `json:"slot_identity"`
	RelativePath string `json:"relative_path"`
	SizeBytes    uint64 `json:"size_bytes"`
	ResultSHA256 string `json:"result_sha256"`
}

type AttemptRecord struct {
	Version           int                    `json:"version"`
	SessionIdentity   string                 `json:"session_identity"`
	SessionPlanSHA256 string                 `json:"session_plan_sha256"`
	Transport         string                 `json:"transport"`
	CaseID            ProbeCaseID            `json:"case_id"`
	AttemptNumber     int                    `json:"attempt_number"`
	SlotIdentity      string                 `json:"slot_identity"`
	AcquisitionID     string                 `json:"acquisition_id"`
	Status            AttemptStatus          `json:"status"`
	Artifact          *ResultArtifactReceipt `json:"artifact,omitempty"`
	Extraction        *Extraction            `json:"extraction,omitempty"`
	RunnerError       string                 `json:"runner_error,omitempty"`
	ExtractionError   string                 `json:"extraction_error,omitempty"`
	Reason            string                 `json:"reason,omitempty"`
}

type TransportSessionReport struct {
	Transport        string          `json:"transport"`
	Diagnostic       bool            `json:"diagnostic,omitempty"`
	Attempts         []AttemptRecord `json:"attempts,omitempty"`
	Report           Report          `json:"evaluation"`
	Outcome          run.Outcome     `json:"outcome"`
	PromotionOutcome run.Outcome     `json:"promotion_outcome"`
}

type SessionReport struct {
	Version             int                      `json:"version"`
	SessionIdentity     string                   `json:"session_identity"`
	SessionPlanSHA256   string                   `json:"session_plan_sha256"`
	ConfigSHA256        string                   `json:"config_sha256"`
	DoctorSHA256        string                   `json:"doctor_sha256,omitempty"`
	DoctorOK            bool                     `json:"doctor_ok"`
	AttemptLedgerSHA256 string                   `json:"attempt_ledger_sha256"`
	Transports          []TransportSessionReport `json:"transports"`
	Outcome             run.Outcome              `json:"outcome"`
	Promotable          bool                     `json:"promotable"`
	Reasons             []string                 `json:"reasons,omitempty"`
}

type sessionDependencies struct {
	buildPlan func(context.Context, SessionConfig) (SessionPlan, error)
	run       func(context.Context, run.RunConfig) (*run.Result, error)
	extract   func(ExtractRequest) (Extraction, error)
	evaluate  func(Config, string, run.ClassMappingRecord, []EvidenceRun) Report
}

var defaultSessionDependencies = sessionDependencies{
	buildPlan: BuildSessionPlan,
	run:       run.Run,
	extract:   ExtractEvidence,
	evaluate:  Evaluate,
}

// RunSession starts or resumes the fixed conformance acquisition plan in
// config.OutputDir. Attempt receipts are durable before execution, so a crash
// can abandon an attempt but can never silently reuse it.
func RunSession(ctx context.Context, config SessionConfig) (*SessionReport, error) {
	return runSession(ctx, config, defaultSessionDependencies)
}

func runSession(ctx context.Context, config SessionConfig, dependencies sessionDependencies) (_ *SessionReport, returnErr error) {
	if ctx == nil {
		return nil, fmt.Errorf("context is required")
	}
	prepared, err := config.Prepare()
	if err != nil {
		return nil, err
	}
	if dependencies.buildPlan == nil || dependencies.run == nil || dependencies.extract == nil || dependencies.evaluate == nil {
		return nil, fmt.Errorf("session dependencies are incomplete")
	}
	lock, err := acquireSessionLock(prepared.OutputDir)
	if err != nil {
		return nil, err
	}
	defer func() {
		if closeErr := lock.close(); returnErr == nil && closeErr != nil {
			returnErr = closeErr
		}
	}()

	stateDir := filepath.Join(prepared.OutputDir, sessionStateDirectory)
	manifestPath := filepath.Join(stateDir, sessionManifestFile)
	manifestExists, err := fileExists(manifestPath)
	if err != nil {
		return nil, fmt.Errorf("inspect session manifest: %w", err)
	}
	if manifestExists {
		var manifest SessionManifest
		if _, err := readCanonicalJSON(manifestPath, &manifest); err != nil {
			return nil, err
		}
		if err := verifySessionManifest(prepared.OutputDir, manifest); err != nil {
			return nil, err
		}
	}

	currentPlan, err := dependencies.buildPlan(ctx, prepared)
	if err != nil {
		return nil, err
	}
	if err := validateSessionPlanForDriver(currentPlan); err != nil {
		return nil, err
	}
	doctorOK, doctorReason, err := evaluateSessionDoctor(
		prepared.DoctorReport, currentPlan.DoctorSHA256, prepared.ServerCPUs, prepared.ClientCPUs,
	)
	if err != nil {
		return nil, err
	}
	planPath := filepath.Join(stateDir, sessionPlanFile)
	planExists, err := fileExists(planPath)
	if err != nil {
		return nil, fmt.Errorf("inspect session plan: %w", err)
	}
	var plan SessionPlan
	var planBytes []byte
	if planExists {
		planBytes, err = readCanonicalJSON(planPath, &plan)
		if err != nil {
			return nil, err
		}
		if !reflect.DeepEqual(plan, currentPlan) {
			return nil, fmt.Errorf("session plan drift: config, mapping, binary, orchestrator, doctor, or environment changed")
		}
	} else {
		for _, name := range []string{attemptsDirectory, sessionReportFile, sessionManifestFile} {
			exists, inspectErr := fileExists(filepath.Join(stateDir, name))
			if inspectErr != nil {
				return nil, inspectErr
			}
			if exists {
				return nil, fmt.Errorf("session state %s exists before plan", name)
			}
		}
		plan = currentPlan
		planBytes, err = writeExclusiveCanonicalJSON(planPath, plan)
		if err != nil {
			return nil, err
		}
	}
	planSHA := hashCanonicalBytes(planBytes)
	if manifestExists {
		var manifest SessionManifest
		if _, err := readCanonicalJSON(manifestPath, &manifest); err != nil {
			return nil, err
		}
		if manifest.SessionIdentity != plan.SessionIdentity {
			return nil, fmt.Errorf("manifest session identity does not match plan")
		}
	}
	if err := validateSessionStateLayout(prepared.OutputDir, &plan); err != nil {
		return nil, err
	}

	preflights := make(map[string]Preflight, len(plan.Preflights))
	for _, preflight := range plan.Preflights {
		preflights[preflight.Transport] = preflight
	}
	evidenceByTransport := make(map[string][]EvidenceRun, len(preflights))
	recordsByTransport := make(map[string][]AttemptRecord, len(preflights))

	for _, casePlan := range plan.Cases {
		preflight, ok := preflights[casePlan.Transport]
		if !ok {
			return nil, fmt.Errorf("case %s/%s has no preflight", casePlan.Transport, casePlan.ID)
		}
		if casePlan.Unsupported {
			if len(casePlan.Attempts) != 0 {
				return nil, fmt.Errorf("unsupported case %s/%s has attempts", casePlan.Transport, casePlan.ID)
			}
			continue
		}

		dependencyReason := ""
		if !casePlan.Clean {
			cleanID := cleanCaseForClass(casePlan.Class)
			cleanOutcome := caseOutcome(dependencies.evaluate(plan.Config.Probe, casePlan.Transport,
				preflight.Mapping, evidenceByTransport[casePlan.Transport]), cleanID)
			if cleanOutcome != run.OutcomePass {
				dependencyReason = fmt.Sprintf("corresponding clean control %s outcome=%s", cleanID, cleanOutcome)
			}
		}

		accepted := 0
		for _, attempt := range casePlan.Attempts {
			if accepted >= casePlan.RequiredAcquisitions {
				exists, inspectErr := attemptStateExists(prepared.OutputDir, attempt)
				if inspectErr != nil {
					return nil, inspectErr
				}
				if exists {
					return nil, fmt.Errorf("attempt %s/%s/%d was consumed after the fixed acquisition count was reached",
						attempt.Transport, attempt.CaseID, attempt.AttemptNumber)
				}
				break
			}
			record, consumeErr := consumeAttempt(ctx, prepared.OutputDir, plan, planSHA, casePlan,
				preflight, attempt, dependencyReason, dependencies)
			if consumeErr != nil {
				return nil, consumeErr
			}
			recordsByTransport[casePlan.Transport] = append(recordsByTransport[casePlan.Transport], record)
			if record.RunnerError != "" {
				return nil, fmt.Errorf("attempt %s runner error: %s", record.SlotIdentity, record.RunnerError)
			}
			if record.Status == AttemptAccepted || record.Status == AttemptSUTFailure {
				if record.Extraction == nil || record.Extraction.Evidence == nil {
					return nil, fmt.Errorf("accepted attempt %s has no evidence", record.SlotIdentity)
				}
				evidenceByTransport[casePlan.Transport] = append(evidenceByTransport[casePlan.Transport], *record.Extraction.Evidence)
				accepted++
			}
			if ctx.Err() != nil {
				return nil, ctx.Err()
			}
		}
		if accepted > casePlan.RequiredAcquisitions {
			return nil, fmt.Errorf("case %s/%s exceeded required acquisitions", casePlan.Transport, casePlan.ID)
		}
	}

	report := buildSessionReport(plan, planSHA, doctorOK, doctorReason, recordsByTransport, evidenceByTransport, dependencies.evaluate)
	if report.Promotable {
		verifiedEvidence, verifyErr := reverifyPromotionEvidence(prepared.OutputDir, plan, planSHA,
			recordsByTransport, preflights, dependencies)
		if verifyErr != nil {
			return nil, verifyErr
		}
		report = buildSessionReport(plan, planSHA, doctorOK, doctorReason, recordsByTransport,
			verifiedEvidence, dependencies.evaluate)
	}
	reportPath := filepath.Join(stateDir, sessionReportFile)
	if err := persistOrVerifySessionReport(reportPath, report); err != nil {
		return nil, err
	}
	manifest, err := buildSessionManifest(prepared.OutputDir, plan.SessionIdentity)
	if err != nil {
		return nil, err
	}
	if err := validateManifestReceipts(prepared.OutputDir, manifest, plan, recordsByTransport); err != nil {
		return nil, err
	}
	if _, err := writeExclusiveCanonicalJSON(manifestPath, manifest); err != nil {
		return nil, err
	}
	if err := verifySessionManifest(prepared.OutputDir, manifest); err != nil {
		return nil, err
	}
	return &report, nil
}

func reverifyPromotionEvidence(outputDir string, plan SessionPlan, planSHA string,
	recordsByTransport map[string][]AttemptRecord, preflights map[string]Preflight,
	dependencies sessionDependencies,
) (map[string][]EvidenceRun, error) {
	recordsBySlot := make(map[string]AttemptRecord)
	want := 0
	for _, records := range recordsByTransport {
		for _, record := range records {
			if record.Status != AttemptAccepted && record.Status != AttemptSUTFailure {
				continue
			}
			if _, duplicate := recordsBySlot[record.SlotIdentity]; duplicate {
				return nil, fmt.Errorf("duplicate promotable evidence record %s", record.SlotIdentity)
			}
			recordsBySlot[record.SlotIdentity] = record
			want++
		}
	}

	verifiedEvidence := make(map[string][]EvidenceRun, len(preflights))
	verified := 0
	for _, casePlan := range plan.Cases {
		preflight, ok := preflights[casePlan.Transport]
		if !ok {
			return nil, fmt.Errorf("case %s/%s has no preflight during final verification", casePlan.Transport, casePlan.ID)
		}
		for _, attempt := range casePlan.Attempts {
			stored, ok := recordsBySlot[attempt.SlotIdentity]
			if !ok {
				continue
			}
			artifactPath := attemptStatePath(outputDir, attempt, attemptArtifactFile)
			resultPath := filepath.Join(attempt.RunConfig.OutputDir, "result.json")
			rawResultExists, err := fileExists(resultPath)
			if err != nil {
				return nil, err
			}
			rechecked, err := verifyStoredRecord(stored, plan, planSHA, attempt, preflight,
				casePlan, artifactPath, rawResultExists, dependencies)
			if err != nil {
				return nil, fmt.Errorf("final verification: %w", err)
			}
			if rechecked.Extraction == nil || rechecked.Extraction.Evidence == nil {
				return nil, fmt.Errorf("final verification: attempt %s has no evidence", attempt.SlotIdentity)
			}
			verifiedEvidence[casePlan.Transport] = append(verifiedEvidence[casePlan.Transport],
				*rechecked.Extraction.Evidence)
			delete(recordsBySlot, attempt.SlotIdentity)
			verified++
		}
	}
	if verified != want || len(recordsBySlot) != 0 {
		return nil, fmt.Errorf("final verification covered %d of %d promotable evidence records", verified, want)
	}
	return verifiedEvidence, nil
}

func consumeAttempt(ctx context.Context, outputDir string, plan SessionPlan, planSHA string, casePlan CasePlan,
	preflight Preflight, attempt AttemptPlan, dependencyReason string, dependencies sessionDependencies,
) (AttemptRecord, error) {
	receiptPath := attemptStatePath(outputDir, attempt, attemptPlanFile)
	artifactPath := attemptStatePath(outputDir, attempt, attemptArtifactFile)
	recordPath := attemptStatePath(outputDir, attempt, attemptRecordFile)
	resultPath := filepath.Join(attempt.RunConfig.OutputDir, "result.json")
	expectedPlanReceipt := AttemptPlanReceipt{
		Version: 1, SessionIdentity: plan.SessionIdentity, SessionPlanSHA256: planSHA, Attempt: attempt,
	}
	planReceiptExists, err := fileExists(receiptPath)
	if err != nil {
		return AttemptRecord{}, err
	}
	if planReceiptExists {
		var stored AttemptPlanReceipt
		if _, err := readCanonicalJSON(receiptPath, &stored); err != nil {
			return AttemptRecord{}, err
		}
		if !reflect.DeepEqual(stored, expectedPlanReceipt) {
			return AttemptRecord{}, fmt.Errorf("attempt plan receipt drift for %s", attempt.SlotIdentity)
		}
	} else {
		for _, path := range []string{artifactPath, recordPath} {
			if exists, inspectErr := fileExists(path); inspectErr != nil {
				return AttemptRecord{}, inspectErr
			} else if exists {
				return AttemptRecord{}, fmt.Errorf("attempt artifact exists before plan receipt: %s", path)
			}
		}
		if raw, inspectErr := fileExists(resultPath); inspectErr != nil {
			return AttemptRecord{}, inspectErr
		} else if raw {
			return AttemptRecord{}, fmt.Errorf("attempt result exists before plan receipt: %s", resultPath)
		}
		if _, err := writeExclusiveCanonicalJSON(receiptPath, expectedPlanReceipt); err != nil {
			return AttemptRecord{}, err
		}
	}

	if dependencyReason != "" {
		if planReceiptExists {
			if raw, _ := fileExists(filepath.Join(attempt.RunConfig.OutputDir, "result.json")); raw {
				return AttemptRecord{}, fmt.Errorf("dependency-skipped attempt %s has a raw result", attempt.SlotIdentity)
			}
		}
		record := baseAttemptRecord(plan, planSHA, attempt)
		record.Status = AttemptDependencySkip
		record.Reason = dependencyReason
		return persistOrVerifyRecord(recordPath, artifactPath, attempt, preflight, casePlan, record, dependencies)
	}

	artifactExists, err := fileExists(artifactPath)
	if err != nil {
		return AttemptRecord{}, err
	}
	recordExists, err := fileExists(recordPath)
	if err != nil {
		return AttemptRecord{}, err
	}
	rawResultExists, err := fileExists(resultPath)
	if err != nil {
		return AttemptRecord{}, err
	}
	if recordExists {
		var stored AttemptRecord
		if _, err := readCanonicalJSON(recordPath, &stored); err != nil {
			return AttemptRecord{}, err
		}
		return verifyStoredRecord(stored, plan, planSHA, attempt, preflight, casePlan,
			artifactPath, rawResultExists, dependencies)
	}

	if planReceiptExists && !artifactExists && !rawResultExists {
		record := baseAttemptRecord(plan, planSHA, attempt)
		record.Status = AttemptAbandoned
		record.Reason = "attempt plan was durable but no result artifact was produced"
		if _, err := writeExclusiveCanonicalJSON(recordPath, record); err != nil {
			return AttemptRecord{}, err
		}
		return record, nil
	}

	runnerError := ""
	if !planReceiptExists {
		_, runErr := dependencies.run(ctx, attempt.RunConfig)
		if runErr != nil {
			runnerError = runErr.Error()
		}
		rawResultExists, err = fileExists(resultPath)
		if err != nil {
			return AttemptRecord{}, err
		}
		if !rawResultExists {
			record := baseAttemptRecord(plan, planSHA, attempt)
			record.Status = AttemptRunnerError
			record.RunnerError = runnerError
			if record.RunnerError == "" {
				record.RunnerError = "runner returned without result.json"
			}
			if _, err := writeExclusiveCanonicalJSON(recordPath, record); err != nil {
				return AttemptRecord{}, err
			}
			return record, nil
		}
	}

	var artifact ResultArtifactReceipt
	if artifactExists {
		if _, err := readCanonicalJSON(artifactPath, &artifact); err != nil {
			return AttemptRecord{}, err
		}
		if err := verifyArtifactReceipt(resultPath, attempt, artifact); err != nil {
			return AttemptRecord{}, err
		}
	} else {
		artifact, err = captureArtifactReceipt(resultPath, attempt)
		if err != nil {
			return AttemptRecord{}, err
		}
		if _, err := writeExclusiveCanonicalJSON(artifactPath, artifact); err != nil {
			return AttemptRecord{}, err
		}
	}
	extraction, extractionErr := dependencies.extract(extractRequest(plan, casePlan, preflight, attempt, artifact.ResultSHA256))
	record := classifyAttempt(plan, planSHA, attempt, artifact, extraction, runnerError, extractionErr)
	if _, err := writeExclusiveCanonicalJSON(recordPath, record); err != nil {
		return AttemptRecord{}, err
	}
	return record, nil
}

func persistOrVerifyRecord(recordPath, artifactPath string, attempt AttemptPlan, preflight Preflight,
	casePlan CasePlan, expected AttemptRecord, dependencies sessionDependencies,
) (AttemptRecord, error) {
	exists, err := fileExists(recordPath)
	if err != nil {
		return AttemptRecord{}, err
	}
	if !exists {
		if _, err := writeExclusiveCanonicalJSON(recordPath, expected); err != nil {
			return AttemptRecord{}, err
		}
		return expected, nil
	}
	var stored AttemptRecord
	if _, err := readCanonicalJSON(recordPath, &stored); err != nil {
		return AttemptRecord{}, err
	}
	if !reflect.DeepEqual(stored, expected) {
		return AttemptRecord{}, fmt.Errorf("attempt record changed for %s", attempt.SlotIdentity)
	}
	artifactExists, err := fileExists(artifactPath)
	if err != nil {
		return AttemptRecord{}, err
	}
	if artifactExists {
		return AttemptRecord{}, fmt.Errorf("attempt %s has artifact despite status %s", attempt.SlotIdentity, stored.Status)
	}
	return stored, nil
}

func verifyStoredRecord(stored AttemptRecord, plan SessionPlan, planSHA string, attempt AttemptPlan,
	preflight Preflight, casePlan CasePlan, artifactPath string, rawResultExists bool,
	dependencies sessionDependencies,
) (AttemptRecord, error) {
	expectedBase := baseAttemptRecord(plan, planSHA, attempt)
	if stored.Version != expectedBase.Version || stored.SessionIdentity != expectedBase.SessionIdentity ||
		stored.SessionPlanSHA256 != expectedBase.SessionPlanSHA256 || stored.Transport != attempt.Transport ||
		stored.CaseID != attempt.CaseID || stored.AttemptNumber != attempt.AttemptNumber ||
		stored.SlotIdentity != attempt.SlotIdentity || stored.AcquisitionID != attempt.AcquisitionID {
		return AttemptRecord{}, fmt.Errorf("attempt record binding drift for %s", attempt.SlotIdentity)
	}
	artifactExists, err := fileExists(artifactPath)
	if err != nil {
		return AttemptRecord{}, err
	}
	switch stored.Status {
	case AttemptAccepted, AttemptInvalid:
		if !artifactExists || stored.Artifact == nil || stored.Extraction == nil || !rawResultExists {
			return AttemptRecord{}, fmt.Errorf("attempt %s status=%s lacks artifact/extraction", attempt.SlotIdentity, stored.Status)
		}
		if stored.RunnerError != "" || stored.Reason != "" {
			return AttemptRecord{}, fmt.Errorf("attempt %s status=%s has incompatible runner error/reason", attempt.SlotIdentity, stored.Status)
		}
	case AttemptSUTFailure:
		if !artifactExists || stored.Artifact == nil || stored.Extraction == nil || !rawResultExists {
			return AttemptRecord{}, fmt.Errorf("attempt %s status=%s lacks artifact/extraction", attempt.SlotIdentity, stored.Status)
		}
		if stored.Reason != "" {
			return AttemptRecord{}, fmt.Errorf("attempt %s status=%s has incompatible reason", attempt.SlotIdentity, stored.Status)
		}
	case AttemptRunnerError:
		if stored.RunnerError == "" {
			return AttemptRecord{}, fmt.Errorf("runner_error attempt %s lacks runner_error", attempt.SlotIdentity)
		}
		if !artifactExists && (stored.Artifact != nil || stored.Extraction != nil || stored.ExtractionError != "") {
			return AttemptRecord{}, fmt.Errorf("runner_error attempt %s has dangling extraction state", attempt.SlotIdentity)
		}
	case AttemptAbandoned:
		if artifactExists || rawResultExists || stored.Artifact != nil || stored.Extraction != nil ||
			stored.RunnerError != "" || stored.ExtractionError != "" || stored.Reason == "" {
			return AttemptRecord{}, fmt.Errorf("abandoned attempt %s gained an artifact", attempt.SlotIdentity)
		}
		return stored, nil
	case AttemptDependencySkip:
		return AttemptRecord{}, fmt.Errorf("unexpected dependency-skipped attempt %s", attempt.SlotIdentity)
	default:
		return AttemptRecord{}, fmt.Errorf("attempt %s has unknown status %q", attempt.SlotIdentity, stored.Status)
	}
	if !artifactExists {
		if rawResultExists {
			return AttemptRecord{}, fmt.Errorf("attempt %s has unreceipted result artifact", attempt.SlotIdentity)
		}
		return stored, nil
	}
	var artifact ResultArtifactReceipt
	if _, err := readCanonicalJSON(artifactPath, &artifact); err != nil {
		return AttemptRecord{}, err
	}
	if stored.Artifact == nil || !reflect.DeepEqual(*stored.Artifact, artifact) {
		return AttemptRecord{}, fmt.Errorf("attempt %s record/artifact receipt mismatch", attempt.SlotIdentity)
	}
	resultPath := filepath.Join(attempt.RunConfig.OutputDir, "result.json")
	if err := verifyArtifactReceipt(resultPath, attempt, artifact); err != nil {
		return AttemptRecord{}, err
	}
	extraction, extractionErr := dependencies.extract(extractRequest(plan, casePlan, preflight, attempt, artifact.ResultSHA256))
	if stored.Extraction == nil || !reflect.DeepEqual(*stored.Extraction, extraction) {
		return AttemptRecord{}, fmt.Errorf("attempt %s extraction changed on resume", attempt.SlotIdentity)
	}
	actualExtractionError := ""
	if extractionErr != nil {
		actualExtractionError = extractionErr.Error()
	}
	if stored.ExtractionError != actualExtractionError {
		return AttemptRecord{}, fmt.Errorf("attempt %s extraction error changed on resume", attempt.SlotIdentity)
	}
	expectedStatus := classifyAttemptStatus(extraction, stored.RunnerError, extractionErr)
	if stored.Status != expectedStatus {
		return AttemptRecord{}, fmt.Errorf("attempt %s status=%s, re-extraction implies %s", attempt.SlotIdentity, stored.Status, expectedStatus)
	}
	if stored.Status == AttemptAccepted && len(extraction.Evidence.SUTFailureReasons) != 0 {
		return AttemptRecord{}, fmt.Errorf("accepted attempt %s contains SUT failure evidence", attempt.SlotIdentity)
	}
	if stored.Status == AttemptSUTFailure && len(extraction.Evidence.SUTFailureReasons) == 0 {
		return AttemptRecord{}, fmt.Errorf("sut_failure attempt %s lacks SUT failure evidence", attempt.SlotIdentity)
	}
	return stored, nil
}

func captureArtifactReceipt(resultPath string, attempt AttemptPlan) (ResultArtifactReceipt, error) {
	size, digest, err := regularFileDigest(resultPath, maxResultBytes)
	if err != nil {
		return ResultArtifactReceipt{}, fmt.Errorf("capture result artifact: %w", err)
	}
	return ResultArtifactReceipt{
		Version: 1, SlotIdentity: attempt.SlotIdentity, RelativePath: "result.json",
		SizeBytes: size, ResultSHA256: digest,
	}, nil
}

func verifyArtifactReceipt(resultPath string, attempt AttemptPlan, artifact ResultArtifactReceipt) error {
	if artifact.Version != 1 || artifact.SlotIdentity != attempt.SlotIdentity || artifact.RelativePath != "result.json" ||
		!isSHA256Digest(artifact.ResultSHA256) {
		return fmt.Errorf("invalid result artifact receipt for %s", attempt.SlotIdentity)
	}
	size, digest, err := regularFileDigest(resultPath, maxResultBytes)
	if err != nil {
		return fmt.Errorf("verify result artifact: %w", err)
	}
	if size != artifact.SizeBytes || digest != artifact.ResultSHA256 {
		return fmt.Errorf("result artifact changed for %s", attempt.SlotIdentity)
	}
	return nil
}

func extractRequest(plan SessionPlan, casePlan CasePlan, preflight Preflight, attempt AttemptPlan, resultSHA string) ExtractRequest {
	return ExtractRequest{
		RunDir: attempt.RunConfig.OutputDir, CaseID: attempt.CaseID, AttemptNumber: attempt.AttemptNumber,
		AcquisitionID: attempt.AcquisitionID, Config: plan.Config.Probe, Mapping: preflight.Mapping,
		ExpectedTransport: attempt.Transport, ExpectedRunConfig: &attempt.RunConfig,
		ExpectedRunIdentity: attempt.RunIdentity, ExpectedResultSHA256: resultSHA,
		ExpectedProbeIdentity: casePlan.ProbeIdentity, ExpectedCaseIdentity: casePlan.CaseIdentity,
	}
}

func baseAttemptRecord(plan SessionPlan, planSHA string, attempt AttemptPlan) AttemptRecord {
	return AttemptRecord{
		Version: AttemptRecordVersion, SessionIdentity: plan.SessionIdentity, SessionPlanSHA256: planSHA,
		Transport: attempt.Transport, CaseID: attempt.CaseID, AttemptNumber: attempt.AttemptNumber,
		SlotIdentity: attempt.SlotIdentity, AcquisitionID: attempt.AcquisitionID,
	}
}

func classifyAttempt(plan SessionPlan, planSHA string, attempt AttemptPlan, artifact ResultArtifactReceipt,
	extraction Extraction, runnerError string, extractionErr error,
) AttemptRecord {
	record := baseAttemptRecord(plan, planSHA, attempt)
	record.Artifact = &artifact
	record.Extraction = &extraction
	record.RunnerError = runnerError
	if extractionErr != nil {
		record.ExtractionError = extractionErr.Error()
	}
	record.Status = classifyAttemptStatus(extraction, runnerError, extractionErr)
	return record
}

func classifyAttemptStatus(extraction Extraction, runnerError string, extractionErr error) AttemptStatus {
	extractionStatus := classifyExtractionStatus(extraction, extractionErr)
	if extractionStatus == AttemptSUTFailure {
		return AttemptSUTFailure
	}
	if runnerError != "" {
		return AttemptRunnerError
	}
	return extractionStatus
}

func classifyExtractionStatus(extraction Extraction, extractionErr error) AttemptStatus {
	if extractionErr != nil || len(extraction.InvalidReasons) != 0 || extraction.Evidence == nil {
		return AttemptInvalid
	}
	if len(extraction.Evidence.SUTFailureReasons) != 0 {
		return AttemptSUTFailure
	}
	return AttemptAccepted
}

func attemptStateExists(outputDir string, attempt AttemptPlan) (bool, error) {
	return fileExists(attemptStatePath(outputDir, attempt, attemptPlanFile))
}

func cleanCaseForClass(class string) ProbeCaseID {
	switch class {
	case run.ClassLossTolerant:
		return CaseCleanLossTolerant
	case run.ClassMustDeliver:
		return CaseCleanMustDeliver
	default:
		return ""
	}
}

func caseOutcome(report Report, caseID ProbeCaseID) run.Outcome {
	for _, caseReport := range report.Cases {
		if caseReport.ID == caseID {
			return caseReport.Outcome
		}
	}
	return run.OutcomeInvalid
}

func buildSessionReport(plan SessionPlan, planSHA string, doctorOK bool, doctorReason string,
	recordsByTransport map[string][]AttemptRecord,
	evidenceByTransport map[string][]EvidenceRun,
	evaluate func(Config, string, run.ClassMappingRecord, []EvidenceRun) Report,
) SessionReport {
	preflights := append([]Preflight(nil), plan.Preflights...)
	sort.Slice(preflights, func(i, j int) bool { return preflights[i].Transport < preflights[j].Transport })
	report := SessionReport{
		Version: SessionReportVersion, SessionIdentity: plan.SessionIdentity, SessionPlanSHA256: planSHA,
		ConfigSHA256: plan.ConfigSHA256, DoctorSHA256: plan.DoctorSHA256, DoctorOK: doctorOK, Outcome: run.OutcomePass,
	}
	if doctorReason != "" {
		report.Reasons = append(report.Reasons, doctorReason)
	}
	var ledger []AttemptRecord
	for _, preflight := range preflights {
		records := append([]AttemptRecord(nil), recordsByTransport[preflight.Transport]...)
		sort.Slice(records, func(i, j int) bool {
			if records[i].CaseID != records[j].CaseID {
				return records[i].CaseID < records[j].CaseID
			}
			return records[i].AttemptNumber < records[j].AttemptNumber
		})
		evaluation := evaluate(plan.Config.Probe, preflight.Transport, preflight.Mapping,
			evidenceByTransport[preflight.Transport])
		transportConfig := plan.Config.Transports[preflight.Transport]
		promotionOutcome := evaluation.Outcome
		if transportConfig.Diagnostic {
			promotionOutcome = diagnosticPromotionOutcome(evaluation)
		}
		report.Transports = append(report.Transports, TransportSessionReport{
			Transport: preflight.Transport, Diagnostic: transportConfig.Diagnostic,
			Attempts: records, Report: evaluation, Outcome: evaluation.Outcome,
			PromotionOutcome: promotionOutcome,
		})
		ledger = append(ledger, records...)
		report.Outcome = worseOutcome(report.Outcome, promotionOutcome)
		if promotionOutcome != run.OutcomePass {
			report.Reasons = append(report.Reasons, fmt.Sprintf("transport %s promotion_outcome=%s", preflight.Transport, promotionOutcome))
		}
	}
	report.AttemptLedgerSHA256 = run.HashValue(ledger)
	report.Promotable = report.Outcome == run.OutcomePass && report.DoctorOK
	sort.Strings(report.Reasons)
	return report
}

func diagnosticPromotionOutcome(report Report) run.Outcome {
	if report.Outcome == run.OutcomeInvalid || report.Outcome == run.OutcomeFail {
		return report.Outcome
	}
	hasPass := false
	outcome := run.OutcomePass
	for _, item := range report.Cases {
		switch item.Outcome {
		case run.OutcomePass:
			hasPass = true
		case run.OutcomeUnsupported:
		default:
			outcome = worseOutcome(outcome, item.Outcome)
		}
	}
	if !hasPass && outcome == run.OutcomePass {
		return run.OutcomeUnsupported
	}
	return outcome
}

func validatePlanOutputPath(outputDir string, attempt AttemptPlan) error {
	relative, err := filepath.Rel(outputDir, attempt.RunConfig.OutputDir)
	if err != nil {
		return err
	}
	if relative == "." || relative == "" || relative == ".." || strings.HasPrefix(relative, ".."+string(filepath.Separator)) {
		return fmt.Errorf("attempt output directory escapes session output: %s", attempt.RunConfig.OutputDir)
	}
	return nil
}

func validateAttemptPlan(plan SessionPlan, casePlan CasePlan, attempt AttemptPlan) error {
	expectedSlotIdentity := run.HashValue(struct {
		Contract        string      `json:"contract"`
		SessionIdentity string      `json:"session_identity"`
		CaseIdentity    string      `json:"case_identity"`
		CaseID          ProbeCaseID `json:"case_id"`
		AttemptNumber   int         `json:"attempt_number"`
		RunIdentity     string      `json:"run_identity"`
	}{slotIdentityV1, plan.SessionIdentity, casePlan.CaseIdentity, attempt.CaseID, attempt.AttemptNumber, attempt.RunIdentity})
	if attempt.Transport != casePlan.Transport || attempt.CaseID != casePlan.ID || attempt.AttemptNumber <= 0 ||
		attempt.RunIdentity != run.ConfigIdentity(attempt.RunConfig) ||
		attempt.AcquisitionID != AcquisitionIdentity(casePlan.CaseIdentity, attempt.AttemptNumber, attempt.RunIdentity) ||
		attempt.SlotIdentity != expectedSlotIdentity {
		return fmt.Errorf("invalid attempt plan binding for %s/%s/%d", attempt.Transport, attempt.CaseID, attempt.AttemptNumber)
	}
	return validatePlanOutputPath(plan.Config.OutputDir, attempt)
}

func validateSessionPlanForDriver(plan SessionPlan) error {
	if plan.Version != SessionPlanVersion || !isSHA256Digest(plan.SessionIdentity) || !isSHA256Digest(plan.ConfigSHA256) ||
		!isSHA256Digest(plan.OrchestratorSHA256) || !isSHA256Digest(plan.EnvironmentSHA256) ||
		(plan.DoctorSHA256 != "" && !isSHA256Digest(plan.DoctorSHA256)) {
		return fmt.Errorf("invalid session plan identity/version")
	}
	if (plan.Config.DoctorReport == "") != (plan.DoctorSHA256 == "") {
		return fmt.Errorf("session plan doctor locator/hash binding mismatch")
	}
	prepared, err := plan.Config.Prepare()
	if err != nil || !reflect.DeepEqual(prepared, plan.Config) {
		return fmt.Errorf("session plan embeds an invalid or non-canonical config")
	}
	identityConfig := cloneSessionConfig(plan.Config)
	identityConfig.OutputDir = ""
	identityConfig.DoctorReport = ""
	if run.HashValue(identityConfig) != plan.ConfigSHA256 {
		return fmt.Errorf("session plan config hash mismatch")
	}
	if len(plan.Preflights) != len(plan.Config.Transports) {
		return fmt.Errorf("session plan preflight count mismatch")
	}
	preflightByTransport := make(map[string]Preflight, len(plan.Preflights))
	for index, preflight := range plan.Preflights {
		if index > 0 && plan.Preflights[index-1].Transport >= preflight.Transport {
			return fmt.Errorf("session plan preflights are not strictly sorted")
		}
		if _, ok := plan.Config.Transports[preflight.Transport]; !ok {
			return fmt.Errorf("session plan has unknown preflight transport %q", preflight.Transport)
		}
		if preflight.MappingSHA256 != MappingSHA256(preflight.Mapping) ||
			preflight.ProbeIdentity != ProbeIdentity(plan.Config.Probe, preflight.Transport, preflight.MappingSHA256) ||
			!isSHA256Digest(preflight.ServerSHA256) || !isSHA256Digest(preflight.ClientSHA256) {
			return fmt.Errorf("session plan preflight binding mismatch for %q", preflight.Transport)
		}
		preflightByTransport[preflight.Transport] = preflight
	}
	expectedSessionIdentity := run.HashValue(struct {
		Contract           string      `json:"contract"`
		ConfigSHA256       string      `json:"config_sha256"`
		DoctorSHA256       string      `json:"doctor_sha256,omitempty"`
		OrchestratorSHA256 string      `json:"orchestrator_sha256"`
		EnvironmentSHA256  string      `json:"environment_sha256"`
		Preflights         []Preflight `json:"preflights"`
	}{sessionIdentityV1, plan.ConfigSHA256, plan.DoctorSHA256, plan.OrchestratorSHA256, plan.EnvironmentSHA256, plan.Preflights})
	if expectedSessionIdentity != plan.SessionIdentity {
		return fmt.Errorf("session plan identity mismatch")
	}
	if len(plan.Cases) != len(plan.Preflights)*len(RequiredCases()) {
		return fmt.Errorf("session plan case count mismatch")
	}
	seen := map[string]bool{}
	seenCases := map[string]bool{}
	for caseIndex, casePlan := range plan.Cases {
		if caseIndex > 0 {
			previous := plan.Cases[caseIndex-1]
			if previous.Transport > casePlan.Transport || (previous.Transport == casePlan.Transport && previous.ID >= casePlan.ID) {
				return fmt.Errorf("session plan cases are not strictly sorted")
			}
		}
		definition, ok := caseDefinition(casePlan.ID)
		if !ok || definition != casePlan.CaseDefinition {
			return fmt.Errorf("session plan has invalid case definition %q", casePlan.ID)
		}
		preflight, ok := preflightByTransport[casePlan.Transport]
		if !ok {
			return fmt.Errorf("session plan case has no preflight for %q", casePlan.Transport)
		}
		caseKey := casePlan.Transport + "/" + string(casePlan.ID)
		if seenCases[caseKey] {
			return fmt.Errorf("session plan has duplicate case %s", caseKey)
		}
		seenCases[caseKey] = true
		if casePlan.MappingSHA256 != preflight.MappingSHA256 || casePlan.ProbeIdentity != preflight.ProbeIdentity ||
			casePlan.CaseIdentity != CaseIdentity(preflight.ProbeIdentity, definition) ||
			casePlan.EndpointMappingSHA256 != endpointMappingSHA256(preflight.Mapping, definition, preflight.MappingSHA256) ||
			casePlan.RequiredAcquisitions != plan.Config.Probe.ValidAcquisitionsPerCase {
			return fmt.Errorf("session plan case binding mismatch for %s", caseKey)
		}
		_, supported, mappingReasons := mappingForCase(preflight.Mapping, definition)
		if len(mappingReasons) != 0 || casePlan.Unsupported == supported {
			return fmt.Errorf("session plan support binding mismatch for %s", caseKey)
		}
		if casePlan.Unsupported && len(casePlan.Attempts) != 0 {
			return fmt.Errorf("unsupported session plan case %s has attempts", caseKey)
		}
		if !casePlan.Unsupported && len(casePlan.Attempts) != plan.Config.Probe.MaxAttemptsPerCase {
			return fmt.Errorf("session plan case %s attempt count mismatch", caseKey)
		}
		for index, attempt := range casePlan.Attempts {
			if attempt.AttemptNumber != index+1 {
				return fmt.Errorf("attempt numbering gap in plan for %s/%s", casePlan.Transport, casePlan.ID)
			}
			if err := validateAttemptPlan(plan, casePlan, attempt); err != nil {
				return err
			}
			if seen[attempt.SlotIdentity] {
				return fmt.Errorf("duplicate slot identity %s", attempt.SlotIdentity)
			}
			seen[attempt.SlotIdentity] = true
		}
	}
	return nil
}
