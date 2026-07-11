package conformance

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"reflect"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/doctor"
	"github.com/neguse/rudp-bench/orchestrator/rig"
	"github.com/neguse/rudp-bench/orchestrator/run"
)

func TestRunSessionPersistsReceiptsAndDoesNotRerunOnResume(t *testing.T) {
	config, plan := driverTestPlan(t)
	runCalls := 0
	extractCalls := 0
	dependencies := driverTestDependencies(t, plan, &runCalls, &extractCalls, passingDriverEvaluation)

	report, err := runSession(context.Background(), config, dependencies)
	if err != nil {
		t.Fatal(err)
	}
	if report.Outcome != run.OutcomePass || !report.Promotable {
		t.Fatalf("outcome=%s promotable=%v", report.Outcome, report.Promotable)
	}
	if !report.DoctorOK {
		t.Fatal("PASS doctor report was not retained in the session report")
	}
	wantRuns := len(plan.Cases)
	if runCalls != wantRuns || extractCalls != 2*wantRuns {
		t.Fatalf("run calls=%d extract calls=%d, want %d runs and %d extracts", runCalls, extractCalls, wantRuns, 2*wantRuns)
	}
	for _, casePlan := range plan.Cases {
		attempt := casePlan.Attempts[0]
		for _, name := range []string{attemptPlanFile, attemptArtifactFile, attemptRecordFile} {
			if _, err := os.Stat(attemptStatePath(config.OutputDir, attempt, name)); err != nil {
				t.Fatalf("missing %s for %s: %v", name, attempt.SlotIdentity, err)
			}
		}
	}
	if _, err := os.Stat(filepath.Join(config.OutputDir, sessionStateDirectory, sessionManifestFile)); err != nil {
		t.Fatalf("missing final manifest: %v", err)
	}
	var manifest SessionManifest
	if _, err := readCanonicalJSON(filepath.Join(config.OutputDir, sessionStateDirectory, sessionManifestFile), &manifest); err != nil {
		t.Fatal(err)
	}
	lockPath := filepath.ToSlash(filepath.Join(sessionStateDirectory, sessionLockFile))
	lockInventoried := false
	for _, entry := range manifest.Entries {
		if entry.Path == lockPath {
			lockInventoried = entry.SizeBytes == 0 && entry.SHA256 == run.HashBytes(nil)
		}
	}
	if !lockInventoried {
		t.Fatal("manifest does not inventory the stable advisory lock contents")
	}

	resumed, err := runSession(context.Background(), config, dependencies)
	if err != nil {
		t.Fatal(err)
	}
	if runCalls != wantRuns {
		t.Fatalf("resume reran a consumed attempt: calls=%d want=%d", runCalls, wantRuns)
	}
	if extractCalls != 4*wantRuns {
		t.Fatalf("resume did not re-extract and finally verify accepted artifacts: calls=%d want=%d", extractCalls, 4*wantRuns)
	}
	if resumed.AttemptLedgerSHA256 != report.AttemptLedgerSHA256 {
		t.Fatal("resume changed the attempt ledger")
	}
}

func TestRunSessionMarksPlanOnlyAttemptAbandonedAndUsesNextSlot(t *testing.T) {
	config, plan := driverTestPlan(t)
	firstCase := &plan.Cases[0]
	if len(firstCase.Attempts) < 2 {
		t.Fatal("test plan needs two attempts")
	}
	if err := ensureDirectory(filepath.Join(config.OutputDir, sessionStateDirectory)); err != nil {
		t.Fatal(err)
	}
	planBytes, err := writeExclusiveCanonicalJSON(filepath.Join(config.OutputDir, sessionStateDirectory, sessionPlanFile), plan)
	if err != nil {
		t.Fatal(err)
	}
	first := firstCase.Attempts[0]
	receipt := AttemptPlanReceipt{
		Version: 1, SessionIdentity: plan.SessionIdentity,
		SessionPlanSHA256: hashCanonicalBytes(planBytes), Attempt: first,
	}
	if _, err := writeExclusiveCanonicalJSON(attemptStatePath(config.OutputDir, first, attemptPlanFile), receipt); err != nil {
		t.Fatal(err)
	}

	runCalls := 0
	extractCalls := 0
	dependencies := driverTestDependencies(t, plan, &runCalls, &extractCalls, passingDriverEvaluation)
	report, err := runSession(context.Background(), config, dependencies)
	if err != nil {
		t.Fatal(err)
	}
	records := report.Transports[0].Attempts
	var firstRecord, secondRecord *AttemptRecord
	for index := range records {
		record := &records[index]
		if record.CaseID == first.CaseID && record.AttemptNumber == 1 {
			firstRecord = record
		}
		if record.CaseID == first.CaseID && record.AttemptNumber == 2 {
			secondRecord = record
		}
	}
	if firstRecord == nil || firstRecord.Status != AttemptAbandoned {
		t.Fatalf("first record=%+v, want abandoned", firstRecord)
	}
	if secondRecord == nil || secondRecord.Status != AttemptAccepted {
		t.Fatalf("second record=%+v, want accepted", secondRecord)
	}
	if runCalls != len(plan.Cases) {
		t.Fatalf("run calls=%d, want one per case=%d (abandoned slot must not rerun)", runCalls, len(plan.Cases))
	}
}

func TestRunSessionReextractsReceiptedArtifactWithoutRerunning(t *testing.T) {
	config, plan := driverTestPlan(t)
	first := plan.Cases[0].Attempts[0]
	if err := ensureDirectory(filepath.Join(config.OutputDir, sessionStateDirectory)); err != nil {
		t.Fatal(err)
	}
	planBytes, err := writeExclusiveCanonicalJSON(filepath.Join(config.OutputDir, sessionStateDirectory, sessionPlanFile), plan)
	if err != nil {
		t.Fatal(err)
	}
	planReceipt := AttemptPlanReceipt{
		Version: 1, SessionIdentity: plan.SessionIdentity,
		SessionPlanSHA256: hashCanonicalBytes(planBytes), Attempt: first,
	}
	if _, err := writeExclusiveCanonicalJSON(attemptStatePath(config.OutputDir, first, attemptPlanFile), planReceipt); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(first.RunConfig.OutputDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(first.RunConfig.OutputDir, "result.json"), []byte(`{"recovered":true}`), 0o600); err != nil {
		t.Fatal(err)
	}
	artifact, err := captureArtifactReceipt(filepath.Join(first.RunConfig.OutputDir, "result.json"), first)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := writeExclusiveCanonicalJSON(attemptStatePath(config.OutputDir, first, attemptArtifactFile), artifact); err != nil {
		t.Fatal(err)
	}

	runCalls, extractCalls := 0, 0
	dependencies := driverTestDependencies(t, plan, &runCalls, &extractCalls, passingDriverEvaluation)
	report, err := runSession(context.Background(), config, dependencies)
	if err != nil {
		t.Fatal(err)
	}
	if runCalls != len(plan.Cases)-1 {
		t.Fatalf("runner calls=%d, want %d; receipted artifact was rerun", runCalls, len(plan.Cases)-1)
	}
	if extractCalls != 2*len(plan.Cases) {
		t.Fatalf("extract calls=%d, want %d", extractCalls, 2*len(plan.Cases))
	}
	found := false
	for _, record := range report.Transports[0].Attempts {
		if record.SlotIdentity == first.SlotIdentity {
			found = record.Status == AttemptAccepted
		}
	}
	if !found {
		t.Fatal("receipted result was not accepted after re-extraction")
	}
}

func TestRunSessionStopsAfterPersistingCanceledAttemptRegardlessOfStatus(t *testing.T) {
	config, plan := driverTestPlan(t)
	runCalls, extractCalls := 0, 0
	dependencies := driverTestDependencies(t, plan, &runCalls, &extractCalls, passingDriverEvaluation)
	ctx, cancel := context.WithCancel(context.Background())
	originalRun := dependencies.run
	dependencies.run = func(ctx context.Context, config run.RunConfig) (*run.Result, error) {
		result, err := originalRun(ctx, config)
		cancel()
		return result, err
	}
	if _, err := runSession(ctx, config, dependencies); !errors.Is(err, context.Canceled) {
		t.Fatalf("runSession error=%v, want context canceled", err)
	}
	if runCalls != 1 || extractCalls != 1 {
		t.Fatalf("canceled session consumed extra attempts: run=%d extract=%d", runCalls, extractCalls)
	}
	first := plan.Cases[0].Attempts[0]
	if _, err := os.Stat(attemptStatePath(config.OutputDir, first, attemptRecordFile)); err != nil {
		t.Fatalf("canceled attempt record was not persisted: %v", err)
	}
	if _, err := os.Stat(filepath.Join(config.OutputDir, sessionStateDirectory, sessionManifestFile)); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("canceled session unexpectedly finalized a manifest: %v", err)
	}
}

func TestRunSessionPersistsSUTFailureAndStopsOnRunnerError(t *testing.T) {
	config, plan := driverTestPlanWithDoctor(t, nil)
	runCalls, extractCalls := 0, 0
	dependencies := driverTestDependencies(t, plan, &runCalls, &extractCalls, passingDriverEvaluation)
	originalRun := dependencies.run
	dependencies.run = func(ctx context.Context, config run.RunConfig) (*run.Result, error) {
		result, err := originalRun(ctx, config)
		if err != nil {
			return result, err
		}
		return result, errors.New("post-run cleanup failed")
	}
	originalExtract := dependencies.extract
	dependencies.extract = func(request ExtractRequest) (Extraction, error) {
		extraction, err := originalExtract(request)
		if err == nil && extraction.Evidence != nil {
			extraction.Evidence.SUTFailureReasons = []string{"endpoint exited"}
		}
		return extraction, err
	}

	if _, err := runSession(context.Background(), config, dependencies); err == nil ||
		!strings.Contains(err.Error(), "post-run cleanup failed") {
		t.Fatalf("runSession error=%v, want persisted runner error", err)
	}
	if runCalls != 1 || extractCalls != 1 {
		t.Fatalf("runner error consumed another attempt: run=%d extract=%d", runCalls, extractCalls)
	}
	first := plan.Cases[0].Attempts[0]
	var stored AttemptRecord
	if _, err := readCanonicalJSON(attemptStatePath(config.OutputDir, first, attemptRecordFile), &stored); err != nil {
		t.Fatal(err)
	}
	if stored.Status != AttemptSUTFailure || stored.RunnerError != "post-run cleanup failed" ||
		stored.Extraction == nil || stored.Extraction.Evidence == nil ||
		len(stored.Extraction.Evidence.SUTFailureReasons) == 0 {
		t.Fatalf("stored attempt=%+v", stored)
	}
	second := plan.Cases[0].Attempts[1]
	if _, err := os.Stat(attemptStatePath(config.OutputDir, second, attemptPlanFile)); !errors.Is(err, os.ErrNotExist) {
		t.Fatalf("runner error consumed retry slot: %v", err)
	}

	if _, err := runSession(context.Background(), config, dependencies); err == nil ||
		!strings.Contains(err.Error(), "post-run cleanup failed") {
		t.Fatalf("resume error=%v, want same persisted runner error", err)
	}
	if runCalls != 1 || extractCalls != 2 {
		t.Fatalf("resume did not reclassify without rerun: run=%d extract=%d", runCalls, extractCalls)
	}

	stored.Status = AttemptRunnerError
	tampered, err := canonicalJSON(stored)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(attemptStatePath(config.OutputDir, first, attemptRecordFile), tampered, 0o600); err != nil {
		t.Fatal(err)
	}
	if _, err := runSession(context.Background(), config, dependencies); err == nil ||
		!strings.Contains(err.Error(), "re-extraction implies sut_failure") {
		t.Fatalf("resume accepted runner_error status bypass: %v", err)
	}
	if runCalls != 1 || extractCalls != 3 {
		t.Fatalf("classification audit reran attempt: run=%d extract=%d", runCalls, extractCalls)
	}
}

func TestRunSessionRecordsDependencySkipsWithoutRunningLossCases(t *testing.T) {
	config, plan := driverTestPlan(t)
	runCalls := 0
	extractCalls := 0
	dependencies := driverTestDependencies(t, plan, &runCalls, &extractCalls, failingCleanDriverEvaluation)
	report, err := runSession(context.Background(), config, dependencies)
	if err != nil {
		t.Fatal(err)
	}
	if runCalls != 2 {
		t.Fatalf("runner called %d times, want only two clean cases", runCalls)
	}
	skips := 0
	for _, record := range report.Transports[0].Attempts {
		if record.Status == AttemptDependencySkip {
			skips++
			if !strings.Contains(record.Reason, "clean control") {
				t.Fatalf("dependency reason=%q", record.Reason)
			}
		}
	}
	if want := 4 * config.Probe.MaxAttemptsPerCase; skips != want {
		t.Fatalf("dependency skips=%d, want %d", skips, want)
	}
}

func TestRunSessionRejectsManifestMutationAndPlanDrift(t *testing.T) {
	t.Run("manifest mutation", func(t *testing.T) {
		config, plan := driverTestPlan(t)
		runCalls, extractCalls := 0, 0
		dependencies := driverTestDependencies(t, plan, &runCalls, &extractCalls, passingDriverEvaluation)
		if _, err := runSession(context.Background(), config, dependencies); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(config.OutputDir, "unrecorded"), []byte("mutation"), 0o600); err != nil {
			t.Fatal(err)
		}
		if _, err := runSession(context.Background(), config, dependencies); err == nil || !strings.Contains(err.Error(), "manifest") {
			t.Fatalf("mutation error=%v", err)
		}
	})

	t.Run("unknown empty directory", func(t *testing.T) {
		config, plan := driverTestPlan(t)
		runCalls, extractCalls := 0, 0
		dependencies := driverTestDependencies(t, plan, &runCalls, &extractCalls, passingDriverEvaluation)
		if _, err := runSession(context.Background(), config, dependencies); err != nil {
			t.Fatal(err)
		}
		if err := os.Mkdir(filepath.Join(config.OutputDir, "unrecorded-empty-dir"), 0o700); err != nil {
			t.Fatal(err)
		}
		if _, err := runSession(context.Background(), config, dependencies); err == nil || !strings.Contains(err.Error(), "manifest") {
			t.Fatalf("empty-directory mutation error=%v", err)
		}
	})

	t.Run("plan drift", func(t *testing.T) {
		config, plan := driverTestPlan(t)
		runCalls, extractCalls := 0, 0
		dependencies := driverTestDependencies(t, plan, &runCalls, &extractCalls, passingDriverEvaluation)
		if _, err := runSession(context.Background(), config, dependencies); err != nil {
			t.Fatal(err)
		}
		drifted := plan
		drifted.DoctorSHA256 = run.HashValue("changed doctor")
		dependencies.buildPlan = func(context.Context, SessionConfig) (SessionPlan, error) { return drifted, nil }
		if _, err := runSession(context.Background(), config, dependencies); err == nil {
			t.Fatalf("drift error=%v", err)
		}
	})
}

func TestRunSessionRejectsResultMutationAfterFinalReextraction(t *testing.T) {
	config, plan := driverTestPlan(t)
	runCalls, extractCalls := 0, 0
	dependencies := driverTestDependencies(t, plan, &runCalls, &extractCalls, passingDriverEvaluation)
	originalExtract := dependencies.extract
	wrappedCalls := 0
	dependencies.extract = func(request ExtractRequest) (Extraction, error) {
		extraction, err := originalExtract(request)
		wrappedCalls++
		if wrappedCalls == len(plan.Cases)+1 {
			file, openErr := os.OpenFile(filepath.Join(request.RunDir, "result.json"), os.O_APPEND|os.O_WRONLY, 0)
			if openErr != nil {
				t.Fatal(openErr)
			}
			if _, writeErr := file.WriteString(" "); writeErr != nil {
				_ = file.Close()
				t.Fatal(writeErr)
			}
			if closeErr := file.Close(); closeErr != nil {
				t.Fatal(closeErr)
			}
		}
		return extraction, err
	}

	report, err := runSession(context.Background(), config, dependencies)
	if err == nil || !strings.Contains(err.Error(), "manifest result receipt") {
		t.Fatalf("post-extraction mutation error=%v", err)
	}
	if report != nil {
		t.Fatalf("post-extraction mutation returned promotable report: %+v", report)
	}
	manifestPath := filepath.Join(config.OutputDir, sessionStateDirectory, sessionManifestFile)
	if _, statErr := os.Stat(manifestPath); !errors.Is(statErr, os.ErrNotExist) {
		t.Fatalf("post-extraction mutation finalized a manifest: %v", statErr)
	}
}

func TestRunSessionResumesReportOnlyFinalization(t *testing.T) {
	config, plan := driverTestPlan(t)
	runCalls, extractCalls := 0, 0
	dependencies := driverTestDependencies(t, plan, &runCalls, &extractCalls, passingDriverEvaluation)
	first, err := runSession(context.Background(), config, dependencies)
	if err != nil {
		t.Fatal(err)
	}
	manifestPath := filepath.Join(config.OutputDir, sessionStateDirectory, sessionManifestFile)
	if err := os.Remove(manifestPath); err != nil {
		t.Fatal(err)
	}

	resumed, err := runSession(context.Background(), config, dependencies)
	if err != nil {
		t.Fatal(err)
	}
	if runCalls != len(plan.Cases) {
		t.Fatalf("report-only resume reran attempts: calls=%d want=%d", runCalls, len(plan.Cases))
	}
	if extractCalls != 4*len(plan.Cases) {
		t.Fatalf("report-only resume extraction calls=%d want=%d", extractCalls, 4*len(plan.Cases))
	}
	if !reflect.DeepEqual(first, resumed) {
		t.Fatal("report-only resume changed the session report")
	}
	var manifest SessionManifest
	if _, err := readCanonicalJSON(manifestPath, &manifest); err != nil {
		t.Fatal(err)
	}
	if err := verifySessionManifest(config.OutputDir, manifest); err != nil {
		t.Fatalf("report-only resume manifest: %v", err)
	}
}

func TestValidateManifestReceiptsBindsResultAndMetrics(t *testing.T) {
	config, plan := driverTestPlan(t)
	attempt := plan.Cases[0].Attempts[0]
	if err := os.MkdirAll(filepath.Join(attempt.RunConfig.OutputDir, "metrics"), 0o755); err != nil {
		t.Fatal(err)
	}
	resultPath := filepath.Join(attempt.RunConfig.OutputDir, "result.json")
	if err := os.WriteFile(resultPath, []byte(`{"result":true}`), 0o600); err != nil {
		t.Fatal(err)
	}
	metricsRelative := "metrics/server.json"
	metricsPath := filepath.Join(attempt.RunConfig.OutputDir, filepath.FromSlash(metricsRelative))
	metricsBody := []byte(`{"metrics":true}`)
	if err := os.WriteFile(metricsPath, metricsBody, 0o600); err != nil {
		t.Fatal(err)
	}
	artifact, err := captureArtifactReceipt(resultPath, attempt)
	if err != nil {
		t.Fatal(err)
	}
	metricsSize, metricsSHA, err := regularFileDigest(metricsPath, maxSessionJSONBytes)
	if err != nil {
		t.Fatal(err)
	}
	records := map[string][]AttemptRecord{
		attempt.Transport: {{
			SlotIdentity: attempt.SlotIdentity,
			Artifact:     &artifact,
			Extraction: &Extraction{Evidence: &EvidenceRun{
				ResultSHA256: artifact.ResultSHA256,
				MetricsArtifacts: []MetricsArtifactDigest{{
					RelativePath: metricsRelative, SizeBytes: metricsSize, SHA256: metricsSHA,
				}},
			}},
		}},
	}
	manifest, err := buildSessionManifest(config.OutputDir, plan.SessionIdentity)
	if err != nil {
		t.Fatal(err)
	}
	if err := validateManifestReceipts(config.OutputDir, manifest, plan, records); err != nil {
		t.Fatalf("valid receipt linkage: %v", err)
	}

	resultManifestPath, err := sessionManifestPath(config.OutputDir, attempt.RunConfig.OutputDir, artifact.RelativePath)
	if err != nil {
		t.Fatal(err)
	}
	metricsManifestPath, err := sessionManifestPath(config.OutputDir, attempt.RunConfig.OutputDir, metricsRelative)
	if err != nil {
		t.Fatal(err)
	}
	for _, test := range []struct {
		name string
		path string
		want string
	}{
		{name: "result", path: resultManifestPath, want: "result receipt"},
		{name: "metrics", path: metricsManifestPath, want: "metrics receipt"},
	} {
		t.Run(test.name, func(t *testing.T) {
			tampered := manifest
			tampered.Entries = append([]SessionManifestEntry(nil), manifest.Entries...)
			found := false
			for index := range tampered.Entries {
				if tampered.Entries[index].Path == test.path {
					tampered.Entries[index].SHA256 = run.HashValue(test.name + "-tampered")
					found = true
					break
				}
			}
			if !found {
				t.Fatalf("manifest does not contain %s", test.path)
			}
			if err := validateManifestReceipts(config.OutputDir, tampered, plan, records); err == nil ||
				!strings.Contains(err.Error(), test.want) {
				t.Fatalf("tampered %s receipt error=%v", test.name, err)
			}
		})
	}
}

func TestRunSessionDoctorControlsPromotionWithoutBlockingAcquisition(t *testing.T) {
	tests := []struct {
		name       string
		report     *doctor.Report
		wantReason string
	}{
		{name: "absent", wantReason: "not bound"},
		{name: "failed", report: driverDoctorReport(t, false, doctor.StatusFail), wantReason: "outcome is FAIL"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			config, plan := driverTestPlanWithDoctor(t, test.report)
			runCalls, extractCalls := 0, 0
			dependencies := driverTestDependencies(t, plan, &runCalls, &extractCalls, passingDriverEvaluation)
			report, err := runSession(context.Background(), config, dependencies)
			if err != nil {
				t.Fatal(err)
			}
			if report.Outcome != run.OutcomePass || report.DoctorOK || report.Promotable {
				t.Fatalf("outcome=%s doctor_ok=%v promotable=%v", report.Outcome, report.DoctorOK, report.Promotable)
			}
			if !strings.Contains(strings.Join(report.Reasons, "; "), test.wantReason) {
				t.Fatalf("reasons=%v, want %q", report.Reasons, test.wantReason)
			}
			if runCalls == 0 {
				t.Fatal("doctor absence/failure blocked acquisition")
			}
		})
	}
}

func TestRunSessionRejectsInconsistentDoctorBeforeAcquisition(t *testing.T) {
	inconsistent := driverDoctorReport(t, true, doctor.StatusFail)
	config, plan := driverTestPlanWithDoctor(t, inconsistent)
	runCalls, extractCalls := 0, 0
	dependencies := driverTestDependencies(t, plan, &runCalls, &extractCalls, passingDriverEvaluation)
	if _, err := runSession(context.Background(), config, dependencies); err == nil || !strings.Contains(err.Error(), "inconsistent") {
		t.Fatalf("doctor consistency error=%v", err)
	}
	if runCalls != 0 {
		t.Fatalf("runner called %d times with inconsistent doctor", runCalls)
	}
}

func TestRunSessionRejectsForeignOrTamperedDoctorBeforeAcquisition(t *testing.T) {
	t.Run("foreign host", func(t *testing.T) {
		foreign := driverDoctorReport(t, true, doctor.StatusPass)
		foreign.Hostname += "-foreign"
		config, plan := driverTestPlanWithDoctor(t, foreign)
		runCalls, extractCalls := 0, 0
		dependencies := driverTestDependencies(t, plan, &runCalls, &extractCalls, passingDriverEvaluation)
		if _, err := runSession(context.Background(), config, dependencies); err == nil || !strings.Contains(err.Error(), "hostname") {
			t.Fatalf("foreign doctor error=%v", err)
		}
		if runCalls != 0 {
			t.Fatalf("runner called %d times with foreign doctor", runCalls)
		}
	})

	t.Run("tampered after plan", func(t *testing.T) {
		config, plan := driverTestPlan(t)
		data, err := os.ReadFile(config.DoctorReport)
		if err != nil {
			t.Fatal(err)
		}
		data = append(data, ' ')
		if err := os.WriteFile(config.DoctorReport, data, 0o600); err != nil {
			t.Fatal(err)
		}
		runCalls, extractCalls := 0, 0
		dependencies := driverTestDependencies(t, plan, &runCalls, &extractCalls, passingDriverEvaluation)
		if _, err := runSession(context.Background(), config, dependencies); err == nil || !strings.Contains(err.Error(), "changed") {
			t.Fatalf("tampered doctor error=%v", err)
		}
		if runCalls != 0 {
			t.Fatalf("runner called %d times with tampered doctor", runCalls)
		}
	})
}

func TestEvaluateSessionDoctorRequiresCompleteFreshCPUboundReference(t *testing.T) {
	base := driverDoctorReport(t, true, doctor.StatusPass)

	t.Run("stale is retained but not promotable", func(t *testing.T) {
		report := *base
		report.GeneratedAt = time.Now().UTC().Add(-16 * time.Minute)
		path, digest := writeDriverDoctor(t, report)
		ok, reason, err := evaluateSessionDoctor(path, digest, report.Rig.ServerCPUs, report.Rig.ClientCPUs)
		if err != nil || ok || !strings.Contains(reason, "within 15 minutes") {
			t.Fatalf("ok=%v reason=%q error=%v", ok, reason, err)
		}
	})

	t.Run("CPU drift withholds promotion", func(t *testing.T) {
		path, digest := writeDriverDoctor(t, *base)
		ok, reason, err := evaluateSessionDoctor(path, digest, base.Rig.ClientCPUs, base.Rig.ClientCPUs)
		if err != nil || ok || !strings.Contains(reason, "server_cpus") {
			t.Fatalf("ok=%v reason=%q error=%v", ok, reason, err)
		}
	})

	t.Run("synthetic incomplete report is rejected", func(t *testing.T) {
		report := *base
		report.Checks = append([]doctor.Check(nil), base.Checks[:len(base.Checks)-1]...)
		path, digest := writeDriverDoctor(t, report)
		if _, _, err := evaluateSessionDoctor(path, digest, report.Rig.ServerCPUs, report.Rig.ClientCPUs); err == nil || !strings.Contains(err.Error(), "missing required reference check") {
			t.Fatalf("incomplete doctor error=%v", err)
		}
	})
}

func TestClassifyAttemptRetainsRunnerAndSUTFailureStatuses(t *testing.T) {
	_, plan := driverTestPlan(t)
	attempt := plan.Cases[0].Attempts[0]
	artifact := ResultArtifactReceipt{Version: 1, SlotIdentity: attempt.SlotIdentity, RelativePath: "result.json", ResultSHA256: run.HashValue("result")}
	extraction := Extraction{Evidence: &EvidenceRun{SUTFailureReasons: []string{"endpoint exited"}}}
	record := classifyAttempt(plan, run.HashValue(plan), attempt, artifact, extraction, "", nil)
	if record.Status != AttemptSUTFailure {
		t.Fatalf("status=%s, want sut_failure", record.Status)
	}
	record = classifyAttempt(plan, run.HashValue(plan), attempt, artifact, extraction, "runner failed", nil)
	if record.Status != AttemptSUTFailure || record.RunnerError != "runner failed" {
		t.Fatalf("SUT failure did not retain ancillary runner error: %+v", record)
	}
	record = classifyAttempt(plan, run.HashValue(plan), attempt, artifact, extraction, "runner failed", errors.New("extract failed"))
	if record.Status != AttemptRunnerError || record.RunnerError == "" || record.ExtractionError == "" {
		t.Fatalf("runner record=%+v", record)
	}
	record = classifyAttempt(plan, run.HashValue(plan), attempt, artifact, Extraction{Evidence: &EvidenceRun{}}, "runner failed", nil)
	if record.Status != AttemptRunnerError {
		t.Fatalf("accepted extraction with runner error status=%s", record.Status)
	}
	record = classifyAttempt(plan, run.HashValue(plan), attempt, artifact,
		Extraction{InvalidReasons: []string{"invalid evidence"}}, "runner failed", nil)
	if record.Status != AttemptRunnerError {
		t.Fatalf("invalid extraction with runner error status=%s", record.Status)
	}
}

func TestDiagnosticPromotionAllowsDeclaredUnsupportedCasesOnly(t *testing.T) {
	report := Report{Cases: []CaseReport{
		{CaseDefinition: CaseDefinition{ID: CaseCleanLossTolerant}, Outcome: run.OutcomePass},
		{CaseDefinition: CaseDefinition{ID: CaseCleanMustDeliver}, Outcome: run.OutcomeUnsupported},
	}}
	if got := diagnosticPromotionOutcome(report); got != run.OutcomePass {
		t.Fatalf("diagnostic promotion outcome=%s, want PASS", got)
	}
	report.Outcome = run.OutcomeInvalid
	if got := diagnosticPromotionOutcome(report); got != run.OutcomeInvalid {
		t.Fatalf("top-level diagnostic invalidity was hidden: %s", got)
	}
	report.Outcome = ""
	report.Cases[0].Outcome = run.OutcomeInvalid
	if got := diagnosticPromotionOutcome(report); got != run.OutcomeInvalid {
		t.Fatalf("invalid implemented diagnostic case was hidden: %s", got)
	}
	report.Cases[0].Outcome = run.OutcomeUnsupported
	if got := diagnosticPromotionOutcome(report); got != run.OutcomeUnsupported {
		t.Fatalf("fully unsupported diagnostic outcome=%s, want UNSUPPORTED", got)
	}
}

func driverTestPlan(t *testing.T) (SessionConfig, SessionPlan) {
	t.Helper()
	return driverTestPlanWithDoctor(t, driverDoctorReport(t, true, doctor.StatusPass))
}

func driverDoctorReport(t *testing.T, ok bool, status string) *doctor.Report {
	t.Helper()
	host := run.HostEnvironmentSnapshot()
	online, err := rig.ParseCPUSet(host.OnlineCPUs)
	if err != nil || len(online) < 3 {
		t.Fatalf("test host has unusable online CPU set %q", host.OnlineCPUs)
	}
	governors := make(map[string]string, len(host.Governors))
	for cpu, governor := range host.Governors {
		governors[strings.TrimPrefix(cpu, "cpu")] = governor
	}
	var bench []int
	for _, cpu := range online {
		if governors[strconv.Itoa(cpu)] == "performance" {
			bench = append(bench, cpu)
		}
	}
	if len(bench) < 2 {
		t.Skip("reference doctor tests require at least two CPUs with the performance governor")
	}
	benchSet := make(map[int]bool, len(bench))
	for _, cpu := range bench {
		benchSet[cpu] = true
	}
	var osCPUs []int
	for _, cpu := range online {
		if !benchSet[cpu] {
			osCPUs = append(osCPUs, cpu)
		}
	}
	if len(osCPUs) == 0 {
		if len(bench) < 3 {
			t.Skip("reference doctor tests require one OS CPU and two performance benchmark CPUs")
		}
		osCPUs = append(osCPUs, bench[0])
		bench = bench[1:]
	}
	referenceRig := rig.Rig{
		Name: "driver-test", OSCPUs: formatDriverCPUSet(osCPUs), BenchCPUs: formatDriverCPUSet(bench),
		ClientCPUs: formatDriverCPUSet(bench[:len(bench)-1]), ServerCPUs: strconv.Itoa(bench[len(bench)-1]),
		AllCPUs: host.OnlineCPUs, ExpectedClocksource: host.Clocksource,
		RequirePerformanceGovernor: true, RequireIsolation: true, MinNoFile: 1,
	}
	if err := referenceRig.Validate(); err != nil {
		t.Fatalf("construct reference test rig: %v", err)
	}
	var limit syscall.Rlimit
	if err := syscall.Getrlimit(syscall.RLIMIT_NOFILE, &limit); err != nil {
		t.Fatal(err)
	}
	checks := driverDoctorChecks()
	if status != doctor.StatusPass {
		checks[0].Status = status
	}
	return &doctor.Report{
		Version: 1, GeneratedAt: time.Now().UTC(), OK: ok, Rig: referenceRig,
		Hostname: host.Hostname, Architecture: host.Architecture,
		KernelRelease: host.KernelRelease, OnlineCPUs: host.OnlineCPUs, Clocksource: host.Clocksource,
		Governors: governors, NoFileSoft: limit.Cur, NoFileHard: limit.Max,
		Git:                doctor.GitState{Commit: strings.Repeat("a", 40)},
		OrchestratorSHA256: run.OrchestratorFingerprint(),
		Checks:             checks,
	}
}

func driverDoctorChecks() []doctor.Check {
	names := []string{
		"clocksource", "rig_cpu_layout", "bench_cpu_governor", "pid1_cpu_isolation",
		"slice_cpu_isolation_system", "slice_cpu_isolation_user", "slice_cpu_isolation_init.scope",
		"irq_cpu_isolation", "nofile", "residual_netem", "residual_benchmark_netns",
		"tool_ip", "tool_tc", "tool_ethtool", "tool_ping", "tool_iperf3", "tool_jq",
		"tool_sha256sum", "source_state",
	}
	checks := make([]doctor.Check, len(names))
	for index, name := range names {
		checks[index] = doctor.Check{Name: name, Status: doctor.StatusPass}
	}
	return checks
}

func formatDriverCPUSet(cpus []int) string {
	values := make([]string, len(cpus))
	for index, cpu := range cpus {
		values[index] = strconv.Itoa(cpu)
	}
	return strings.Join(values, ",")
}

func writeDriverDoctor(t *testing.T, report doctor.Report) (string, string) {
	t.Helper()
	path := filepath.Join(t.TempDir(), "doctor.json")
	if err := doctor.Write(path, report); err != nil {
		t.Fatal(err)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	return path, run.HashBytes(data)
}

func driverTestPlanWithDoctor(t *testing.T, doctorReport *doctor.Report) (SessionConfig, SessionPlan) {
	t.Helper()
	directory := t.TempDir()
	command := writeDescribeEndpoint(t, directory, "driver-endpoint", "driver", nativeSessionMapping(), "")
	config := validSessionConfig(t, directory, map[string]TransportSpec{
		"driver": {ServerCommand: command, ClientCommand: command, ClientProcs: 1},
	})
	config.Probe.MaxAttemptsPerCase = 2
	if doctorReport != nil {
		config.ServerCPUs = doctorReport.Rig.ServerCPUs
		config.ClientCPUs = doctorReport.Rig.ClientCPUs
		config.DoctorReport = filepath.Join(directory, "doctor.json")
		if err := doctor.Write(config.DoctorReport, *doctorReport); err != nil {
			t.Fatal(err)
		}
	}
	plan := mustBuildSessionPlan(t, config)
	return config, plan
}

func driverTestDependencies(t *testing.T, plan SessionPlan, runCalls, extractCalls *int,
	evaluate func(Config, string, run.ClassMappingRecord, []EvidenceRun) Report,
) sessionDependencies {
	t.Helper()
	return sessionDependencies{
		buildPlan: func(context.Context, SessionConfig) (SessionPlan, error) { return plan, nil },
		run: func(_ context.Context, config run.RunConfig) (*run.Result, error) {
			*runCalls++
			attempt := driverAttemptForOutput(t, plan, config.OutputDir)
			if _, err := os.Stat(attemptStatePath(plan.Config.OutputDir, attempt, attemptPlanFile)); err != nil {
				t.Fatalf("runner started before durable plan receipt: %v", err)
			}
			if err := os.MkdirAll(config.OutputDir, 0o755); err != nil {
				return nil, err
			}
			body := []byte(fmt.Sprintf(`{"slot":%q}`, attempt.SlotIdentity))
			if err := os.WriteFile(filepath.Join(config.OutputDir, "result.json"), body, 0o600); err != nil {
				return nil, err
			}
			return &run.Result{}, nil
		},
		extract: func(request ExtractRequest) (Extraction, error) {
			*extractCalls++
			attempt := driverAttemptForOutput(t, plan, request.RunDir)
			if _, err := os.Stat(attemptStatePath(plan.Config.OutputDir, attempt, attemptArtifactFile)); err != nil {
				t.Fatalf("extraction started before durable artifact receipt: %v", err)
			}
			return Extraction{
				ResultSHA256: request.ExpectedResultSHA256,
				Evidence: &EvidenceRun{
					CaseID: request.CaseID, Transport: request.ExpectedTransport,
					RunIdentity: request.ExpectedRunIdentity, AcquisitionID: request.AcquisitionID,
					AttemptNumber: request.AttemptNumber, ResultSHA256: request.ExpectedResultSHA256,
					MeasurementValid: true,
				},
			}, nil
		},
		evaluate: evaluate,
	}
}

func driverAttemptForOutput(t *testing.T, plan SessionPlan, output string) AttemptPlan {
	t.Helper()
	for _, casePlan := range plan.Cases {
		for _, attempt := range casePlan.Attempts {
			if attempt.RunConfig.OutputDir == output {
				return attempt
			}
		}
	}
	t.Fatalf("no attempt for output %s", output)
	return AttemptPlan{}
}

func passingDriverEvaluation(config Config, transport string, mapping run.ClassMappingRecord, evidence []EvidenceRun) Report {
	report := Report{Version: ReportVersion, GateVersion: GateVersion, Config: config, Transport: transport, EndpointMappings: mapping, Outcome: run.OutcomePass}
	for _, definition := range RequiredCases() {
		report.Cases = append(report.Cases, CaseReport{CaseDefinition: definition, Outcome: run.OutcomePass})
	}
	return report
}

func failingCleanDriverEvaluation(config Config, transport string, mapping run.ClassMappingRecord, evidence []EvidenceRun) Report {
	report := passingDriverEvaluation(config, transport, mapping, evidence)
	report.Outcome = run.OutcomeFail
	for index := range report.Cases {
		if report.Cases[index].Clean {
			report.Cases[index].Outcome = run.OutcomeFail
		}
	}
	return report
}
