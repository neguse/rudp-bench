package conformance

import (
	"encoding/json"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/control"
	"github.com/neguse/rudp-bench/orchestrator/netops"
	"github.com/neguse/rudp-bench/orchestrator/run"
	"golang.org/x/sys/unix"
)

func TestExtractEvidenceVerifiesRawArtifact(t *testing.T) {
	fixture := newExtractionFixture(t)
	extraction, err := ExtractEvidence(fixture.request)
	if err != nil {
		t.Fatal(err)
	}
	if len(extraction.InvalidReasons) != 0 || extraction.Evidence == nil {
		t.Fatalf("extraction invalid: %+v", extraction)
	}
	evidence := extraction.Evidence
	if evidence.ResultSHA256 != fixture.request.ExpectedResultSHA256 ||
		evidence.RunIdentity != fixture.request.ExpectedRunIdentity ||
		evidence.AcquisitionID != fixture.request.AcquisitionID ||
		evidence.CaseIdentity != fixture.request.ExpectedCaseIdentity {
		t.Fatalf("identity binding was not preserved: %+v", evidence)
	}
	if evidence.Transport != extractionTransport || evidence.Slots != 1000 || evidence.Submitted != 1000 ||
		evidence.ExpectedReceives != 1000 || evidence.DeliveredUnique != 1000 || evidence.EligibleDeliveredUnique != 1000 {
		t.Fatalf("counts were not derived from metrics: %+v", evidence)
	}
	if !evidence.ClassExclusive || !evidence.Echo || !evidence.PayloadPatternVerified ||
		!evidence.WireCompressionDisabled || evidence.ServerCoalescing != "none" ||
		evidence.ClientCoalescing != "none" || !evidence.OffloadsDisabled {
		t.Fatalf("probe contract was not derived: %+v", evidence)
	}
	if len(evidence.MetricsArtifacts) != 2 {
		t.Fatalf("metrics artifact digests=%d, want server and client", len(evidence.MetricsArtifacts))
	}
	for _, artifact := range evidence.MetricsArtifacts {
		data, err := os.ReadFile(filepath.Join(fixture.request.RunDir, filepath.FromSlash(artifact.RelativePath)))
		if err != nil {
			t.Fatal(err)
		}
		if artifact.SizeBytes != uint64(len(data)) || artifact.SHA256 != run.HashBytes(data) {
			t.Fatalf("metrics artifact digest does not match bytes: %+v", artifact)
		}
	}
}

func TestExtractEvidenceAcceptsDisclosedLatestValueWhenSubmissionIsExact(t *testing.T) {
	fixture := newExtractionFixture(t)
	var result run.Result
	readJSONFile(t, fixture.resultPath, &result)
	for _, endpoint := range []*run.CommandDescription{&result.Treatment.Server, &result.Treatment.Client} {
		var description map[string]any
		if err := json.Unmarshal(endpoint.Description, &description); err != nil {
			t.Fatal(err)
		}
		description["coalescing"] = "latest-value"
		changed, err := json.Marshal(description)
		if err != nil {
			t.Fatal(err)
		}
		endpoint.Description = changed
	}
	writeJSONFile(t, fixture.resultPath, result)
	fixture.refreshResultHash(t)
	extraction, err := ExtractEvidence(fixture.request)
	if err != nil || extraction.Evidence == nil || len(extraction.InvalidReasons) != 0 {
		t.Fatalf("latest-value disclosure extraction=%+v err=%v", extraction, err)
	}
	if extraction.Evidence.ServerCoalescing != "latest-value" || extraction.Evidence.ClientCoalescing != "latest-value" {
		t.Fatalf("coalescing disclosure was not retained: %+v", extraction.Evidence)
	}
}

func TestMetricsSnapshotSurvivesPostReadPathReplacement(t *testing.T) {
	fixture := newExtractionFixture(t)
	var result run.Result
	readJSONFile(t, fixture.resultPath, &result)
	processes, identityReasons, completenessReasons := validateProbeProcesses(result, extractionTransport)
	if len(identityReasons) != 0 || len(completenessReasons) != 0 {
		t.Fatalf("invalid process fixture: identity=%v completeness=%v", identityReasons, completenessReasons)
	}
	rootFD, err := unix.Open(fixture.request.RunDir, unix.O_PATH|unix.O_DIRECTORY|unix.O_CLOEXEC, 0)
	if err != nil {
		t.Fatal(err)
	}
	defer unix.Close(rootFD)
	snapshot, err := snapshotMetricsArtifacts(fixture.request.RunDir, rootFD, result.Config.OutputDir, processes)
	if err != nil {
		t.Fatal(err)
	}
	clientDigest := snapshot.digests[1]
	if err := os.Rename(fixture.clientMetricsPath, fixture.clientMetricsPath+".original"); err != nil {
		t.Fatal(err)
	}
	tampered := []byte(`{"version":2,"version":2}`)
	if err := os.WriteFile(fixture.clientMetricsPath, tampered, 0o644); err != nil {
		t.Fatal(err)
	}
	merged, err := validateMetricsSnapshot(snapshot, result, processes)
	if err != nil {
		t.Fatalf("snapshot validation observed post-read path replacement: %v", err)
	}
	metric, ok := merged.TrafficMetric(run.TrafficIDClientInput, run.DirectionRoomRelay, run.ClassLossTolerant)
	if !ok || metric.DeliveredUnique != fixture.request.Config.SlotsPerRun {
		t.Fatalf("snapshot metric changed after path replacement: %+v present=%v", metric, ok)
	}
	if snapshot.clients[0].SHA256() != clientDigest.SHA256 || clientDigest.SHA256 == run.HashBytes(tampered) {
		t.Fatalf("snapshot digest was not bound to the originally read bytes: %+v", clientDigest)
	}
	assertInvalidExtraction(t, fixture.request, "duplicate object key")
}

func TestMetricsArtifactDigestTamperingChangesEvidenceHashAndInvalidDigestFails(t *testing.T) {
	fixture := newExtractionFixture(t)
	extraction, err := ExtractEvidence(fixture.request)
	if err != nil || extraction.Evidence == nil {
		t.Fatalf("extract evidence: extraction=%+v err=%v", extraction, err)
	}
	original := *extraction.Evidence
	tampered := original
	tampered.MetricsArtifacts = append([]MetricsArtifactDigest(nil), original.MetricsArtifacts...)
	tampered.MetricsArtifacts[1].SHA256 = strings.Repeat("f", 64)
	if tampered.MetricsArtifacts[1].SHA256 == original.MetricsArtifacts[1].SHA256 {
		tampered.MetricsArtifacts[1].SHA256 = strings.Repeat("e", 64)
	}
	if evidenceSHA256([]EvidenceRun{original}) == evidenceSHA256([]EvidenceRun{tampered}) {
		t.Fatal("metrics digest tampering did not change evidence hash")
	}
	tampered.MetricsArtifacts[1].SHA256 = "not-a-digest"
	report := Evaluate(fixture.request.Config, extractionTransport, fixture.request.Mapping, []EvidenceRun{tampered})
	item := findCase(t, report, fixture.request.CaseID)
	if item.Outcome != run.OutcomeInvalid || !containsReason(item.Reasons, "metrics_artifacts[1].sha256") {
		t.Fatalf("malformed metrics digest was not rejected: %+v", item)
	}
}

func TestRecordedRunIdentityMatchesRunPackageContract(t *testing.T) {
	fixture := newExtractionFixture(t)
	var result run.Result
	readJSONFile(t, fixture.resultPath, &result)
	result.Treatment.Server.SHA256 = run.CommandFingerprint(result.Config.ServerCommand)
	result.Treatment.Client.SHA256 = run.CommandFingerprint(result.Config.ClientCommand)
	result.Treatment.OrchestratorSHA256 = run.OrchestratorFingerprint()
	result.Treatment.EnvironmentSHA256 = run.EnvironmentFingerprint()
	if got, want := recordedRunIdentity(result), run.ConfigIdentity(result.Config); got != want {
		t.Fatalf("recorded identity=%s, run.ConfigIdentity=%s", got, want)
	}
}

func TestExtractEvidenceRejectsUnknownAndTrailingResultJSON(t *testing.T) {
	t.Run("unknown field", func(t *testing.T) {
		fixture := newExtractionFixture(t)
		var object map[string]any
		readJSONFile(t, fixture.resultPath, &object)
		object["untrusted"] = true
		writeJSONFile(t, fixture.resultPath, object)
		fixture.refreshResultHash(t)
		assertInvalidExtraction(t, fixture.request, "unknown field")
	})

	t.Run("trailing value", func(t *testing.T) {
		fixture := newExtractionFixture(t)
		file, err := os.OpenFile(fixture.resultPath, os.O_APPEND|os.O_WRONLY, 0)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := file.WriteString("\n{}\n"); err != nil {
			t.Fatal(err)
		}
		if err := file.Close(); err != nil {
			t.Fatal(err)
		}
		fixture.refreshResultHash(t)
		assertInvalidExtraction(t, fixture.request, "trailing")
	})

	t.Run("duplicate field", func(t *testing.T) {
		fixture := newExtractionFixture(t)
		data, err := os.ReadFile(fixture.resultPath)
		if err != nil {
			t.Fatal(err)
		}
		data = []byte(strings.Replace(string(data), `{"version":2`, `{"version":2,"version":2`, 1))
		if err := os.WriteFile(fixture.resultPath, data, 0o644); err != nil {
			t.Fatal(err)
		}
		fixture.refreshResultHash(t)
		assertInvalidExtraction(t, fixture.request, "duplicate object key")
	})
}

func TestExtractEvidenceRejectsResultHashAndConfigIdentityDrift(t *testing.T) {
	t.Run("result hash", func(t *testing.T) {
		fixture := newExtractionFixture(t)
		fixture.request.ExpectedResultSHA256 = strings.Repeat("f", 64)
		assertInvalidExtraction(t, fixture.request, "ledger hash")
	})

	t.Run("config", func(t *testing.T) {
		fixture := newExtractionFixture(t)
		expected := *fixture.request.ExpectedRunConfig
		expected.Drain.Duration++
		fixture.request.ExpectedRunConfig = &expected
		assertInvalidExtraction(t, fixture.request, "planned run config")
	})
}

func TestExtractEvidenceRejectsNonFinitePlannedRunConfigWithoutPanic(t *testing.T) {
	fixture := newExtractionFixture(t)
	expected := *fixture.request.ExpectedRunConfig
	scenario := *expected.Scenario
	traffic := *scenario.ClientInput
	traffic.LossTolerant.RateHz = math.NaN()
	scenario.ClientInput = &traffic
	expected.Scenario = &scenario
	fixture.request.ExpectedRunConfig = &expected
	assertInvalidExtraction(t, fixture.request, "planned run config")
}

func TestExtractEvidenceRejectsMissingTinyLossEvidenceWithoutPanic(t *testing.T) {
	fixture := newExtractionFixture(t)
	const tinyLoss = 5e-13
	fixture.request.Config.LossPercent = tinyLoss
	var result run.Result
	readJSONFile(t, fixture.resultPath, &result)
	result.Config.Netem.ClientEgress.LossPercent = tinyLoss
	result.Netem.Pair = pairSpecForRegime(result.Config.Netem)
	result.Netem.LossEvidence = nil
	fixture.request.CaseID = CaseLossTolerantClientLoss
	mappingSHA := MappingSHA256(fixture.request.Mapping)
	fixture.request.ExpectedProbeIdentity = ProbeIdentity(fixture.request.Config, extractionTransport, mappingSHA)
	definition, _ := caseDefinition(fixture.request.CaseID)
	fixture.request.ExpectedCaseIdentity = CaseIdentity(fixture.request.ExpectedProbeIdentity, definition)
	expected := result.Config
	fixture.request.ExpectedRunConfig = &expected
	fixture.request.ExpectedRunIdentity = recordedRunIdentity(result)
	fixture.refreshAcquisitionID()
	writeJSONFile(t, fixture.resultPath, result)
	fixture.refreshResultHash(t)
	assertInvalidExtraction(t, fixture.request, "missing")
}

func TestExtractEvidenceRejectsAttemptOutsideFixedPlan(t *testing.T) {
	for _, item := range []struct {
		name    string
		attempt int
	}{{"zero", 0}, {"over max", DefaultConfig(1).MaxAttemptsPerCase + 1}} {
		t.Run(item.name, func(t *testing.T) {
			fixture := newExtractionFixture(t)
			fixture.request.AttemptNumber = item.attempt
			assertInvalidExtraction(t, fixture.request, "attempt_number")
		})
	}
}

func TestExtractEvidenceRejectsTamperedMetricsAndMapping(t *testing.T) {
	t.Run("raw metrics", func(t *testing.T) {
		fixture := newExtractionFixture(t)
		var metricFile map[string]any
		readJSONFile(t, fixture.clientMetricsPath, &metricFile)
		traffic := metricFile["traffic"].([]any)[0].(map[string]any)
		traffic["submitted"] = float64(999)
		writeJSONFile(t, fixture.clientMetricsPath, metricFile)
		assertInvalidExtraction(t, fixture.request, "metrics evidence")
	})

	t.Run("structured mapping", func(t *testing.T) {
		fixture := newExtractionFixture(t)
		var result run.Result
		readJSONFile(t, fixture.resultPath, &result)
		changed := result.Treatment.ClassMapping.Client[run.ClassLossTolerant]
		changed.Primitive = "tampered"
		result.Treatment.ClassMapping.Client[run.ClassLossTolerant] = changed
		writeJSONFile(t, fixture.resultPath, result)
		fixture.refreshResultHash(t)
		assertInvalidExtraction(t, fixture.request, "class mapping")
	})
}

func TestExtractEvidenceRejectsPacketExposureContractDrift(t *testing.T) {
	t.Run("missing post-run offload evidence", func(t *testing.T) {
		fixture := newExtractionFixture(t)
		var result run.Result
		readJSONFile(t, fixture.resultPath, &result)
		result.Netem.OffloadsAfter = nil
		writeJSONFile(t, fixture.resultPath, result)
		fixture.refreshResultHash(t)
		assertInvalidExtraction(t, fixture.request, "offload evidence after measurement: missing")
	})

	for _, test := range []struct {
		name, field, value, reason string
	}{
		{"payload pattern", "payload_pattern", "periodic-v0", "payload_pattern"},
		{"wire compression", "wire_compression", "gzip", "wire_compression"},
	} {
		t.Run(test.name, func(t *testing.T) {
			fixture := newExtractionFixture(t)
			var result run.Result
			readJSONFile(t, fixture.resultPath, &result)
			var description map[string]any
			if err := json.Unmarshal(result.Treatment.Client.Description, &description); err != nil {
				t.Fatal(err)
			}
			description[test.field] = test.value
			changed, err := json.Marshal(description)
			if err != nil {
				t.Fatal(err)
			}
			result.Treatment.Client.Description = changed
			writeJSONFile(t, fixture.resultPath, result)
			fixture.refreshResultHash(t)
			assertInvalidExtraction(t, fixture.request, test.reason)
		})
	}

	t.Run("link MTU", func(t *testing.T) {
		fixture := newExtractionFixture(t)
		fixture.request.Config.LinkMTUBytes = 1600
		mappingSHA := MappingSHA256(fixture.request.Mapping)
		fixture.request.ExpectedProbeIdentity = ProbeIdentity(fixture.request.Config, extractionTransport, mappingSHA)
		definition, _ := caseDefinition(fixture.request.CaseID)
		fixture.request.ExpectedCaseIdentity = CaseIdentity(fixture.request.ExpectedProbeIdentity, definition)
		fixture.refreshAcquisitionID()
		assertInvalidExtraction(t, fixture.request, "link_mtu_bytes=1500, want 1600")
	})

	t.Run("exact submission gate", func(t *testing.T) {
		fixture := newExtractionFixture(t)
		var result run.Result
		readJSONFile(t, fixture.resultPath, &result)
		result.Config.AttemptedThreshold = 0.99
		expected := result.Config
		fixture.request.ExpectedRunConfig = &expected
		fixture.request.ExpectedRunIdentity = recordedRunIdentity(result)
		fixture.refreshAcquisitionID()
		writeJSONFile(t, fixture.resultPath, result)
		fixture.refreshResultHash(t)
		assertInvalidExtraction(t, fixture.request, "attempted_threshold=0.99, want 1")
	})

	t.Run("observed submission accounting", func(t *testing.T) {
		fixture := newExtractionFixture(t)
		writeMetricFixtureCounts(t, fixture.clientMetricsPath, run.ClassLossTolerant,
			fixture.request.Config.SlotsPerRun, fixture.request.Config.SlotsPerRun-1, fixture.request.Config.SlotsPerRun-1)
		merged, err := run.MergeMetricsFiles([]string{fixture.clientMetricsPath}, 1)
		if err != nil {
			t.Fatal(err)
		}
		var result run.Result
		readJSONFile(t, fixture.resultPath, &result)
		result.Metrics = merged
		writeJSONFile(t, fixture.resultPath, result)
		fixture.refreshResultHash(t)
		assertInvalidExtraction(t, fixture.request, "want exact slots=1000")
	})
}

func TestExtractEvidenceBindsParticipantToProcessPID(t *testing.T) {
	fixture := newExtractionFixture(t)
	var result run.Result
	readJSONFile(t, fixture.resultPath, &result)
	result.Control.Participants[1].Hello.PID++
	writeJSONFile(t, fixture.resultPath, result)
	fixture.refreshResultHash(t)
	assertInvalidExtraction(t, fixture.request, "participant identity")
}

func TestExtractEvidenceRejectsProcessCommandThatBypassesNamespace(t *testing.T) {
	fixture := newExtractionFixture(t)
	var result run.Result
	readJSONFile(t, fixture.resultPath, &result)
	result.Processes[1].Command = []string{"/bin/true"}
	writeJSONFile(t, fixture.resultPath, result)
	fixture.refreshResultHash(t)
	assertInvalidExtraction(t, fixture.request, "planned network namespace")
}

func TestExtractEvidenceRejudgesNetemGateReport(t *testing.T) {
	t.Run("missing", func(t *testing.T) {
		fixture := newExtractionFixture(t)
		var result run.Result
		readJSONFile(t, fixture.resultPath, &result)
		result.Netem.Gate = nil
		writeJSONFile(t, fixture.resultPath, result)
		fixture.refreshResultHash(t)
		assertInvalidExtraction(t, fixture.request, "netem gate report is missing")
	})

	t.Run("zero observed configured loss", func(t *testing.T) {
		fixture := newClientLossExtractionFixture(t)
		var result run.Result
		readJSONFile(t, fixture.resultPath, &result)
		result.Netem.Gate.LossC2SPct = 0
		writeJSONFile(t, fixture.resultPath, result)
		fixture.refreshResultHash(t)
		assertInvalidExtraction(t, fixture.request, "allowed")
	})
}

func TestExtractEvidenceRejectsOutOfRootAndSymlinkMetrics(t *testing.T) {
	t.Run("out of root", func(t *testing.T) {
		fixture := newExtractionFixture(t)
		var result run.Result
		readJSONFile(t, fixture.resultPath, &result)
		for index := range result.Processes {
			if result.Processes[index].Role == "client" {
				result.Processes[index].MetricsOut = filepath.Join(t.TempDir(), "outside.json")
			}
		}
		writeJSONFile(t, fixture.resultPath, result)
		fixture.refreshResultHash(t)
		assertInvalidExtraction(t, fixture.request, "escapes the run directory")
	})

	t.Run("symlink", func(t *testing.T) {
		fixture := newExtractionFixture(t)
		outside := filepath.Join(t.TempDir(), "outside.json")
		data, err := os.ReadFile(fixture.clientMetricsPath)
		if err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(outside, data, 0o644); err != nil {
			t.Fatal(err)
		}
		if err := os.Remove(fixture.clientMetricsPath); err != nil {
			t.Fatal(err)
		}
		if err := os.Symlink(outside, fixture.clientMetricsPath); err != nil {
			t.Fatal(err)
		}
		assertInvalidExtraction(t, fixture.request, "traverses symlink")
	})
}

func TestExtractEvidenceRejectsMismatchedCaseAndClass(t *testing.T) {
	fixture := newExtractionFixture(t)
	fixture.request.CaseID = CaseCleanMustDeliver
	definition, ok := caseDefinition(fixture.request.CaseID)
	if !ok {
		t.Fatal("missing test case definition")
	}
	fixture.request.ExpectedCaseIdentity = CaseIdentity(fixture.request.ExpectedProbeIdentity, definition)
	fixture.refreshAcquisitionID()
	assertInvalidExtraction(t, fixture.request, "opposing traffic class")
}

func TestExtractEvidencePreservesStaticOnlySUTFailure(t *testing.T) {
	fixture := newExtractionFixture(t)
	var result run.Result
	readJSONFile(t, fixture.resultPath, &result)
	result.Processes = result.Processes[:1]
	result.Processes[0].Exited = true
	result.Processes[0].ExitCode = 7
	result.Processes[0].Error = "exit status 7"
	result.Control = nil
	result.Metrics = nil
	result.Verdict = run.VerdictInvalid
	result.InvalidReasons = []string{
		"control result is missing",
		"merged metrics are missing",
		"server proc_index=-1 pid=100 exit_code=7: exit status 7",
	}
	result.Outcome = run.OutcomeInvalid
	result.OutcomeReasons = []string{"server exited before control completion"}
	result.Netem.OffloadsAfter = nil
	writeJSONFile(t, fixture.resultPath, result)
	fixture.refreshResultHash(t)

	extraction, err := ExtractEvidence(fixture.request)
	if err != nil {
		t.Fatal(err)
	}
	if len(extraction.InvalidReasons) != 0 || extraction.Evidence == nil {
		t.Fatalf("static-only crash extraction=%+v", extraction)
	}
	if !containsReason(extraction.Evidence.SUTFailureReasons, "exit_code=7") {
		t.Fatalf("SUT failure was not derived: %+v", extraction.Evidence)
	}
	if extraction.Evidence.Slots != 0 || extraction.Evidence.Loss.Mode != LossModeNone {
		t.Fatalf("dynamic fields were invented for early crash: %+v", extraction.Evidence)
	}

	report := Evaluate(fixture.request.Config, extractionTransport, fixture.request.Mapping, []EvidenceRun{*extraction.Evidence})
	if report.Outcome != run.OutcomeFail {
		t.Fatalf("static-only crash did not remain a SUT failure: %+v", report)
	}
}

func TestExtractEvidenceRecognizesEarlyGracefulExitAsSUTFailure(t *testing.T) {
	fixture := newExtractionFixture(t)
	var result run.Result
	readJSONFile(t, fixture.resultPath, &result)
	result.Processes = result.Processes[:1]
	result.Control = nil
	result.Metrics = nil
	result.Verdict = run.VerdictInvalid
	result.InvalidReasons = []string{
		"control result is missing",
		"merged metrics are missing",
		"server exited before becoming ready: server proc_index=-1 pid=100 exit_code=0",
	}
	result.Outcome = run.OutcomeInvalid
	writeJSONFile(t, fixture.resultPath, result)
	fixture.refreshResultHash(t)

	extraction, err := ExtractEvidence(fixture.request)
	if err != nil {
		t.Fatal(err)
	}
	if len(extraction.InvalidReasons) != 0 || extraction.Evidence == nil ||
		!containsReason(extraction.Evidence.SUTFailureReasons, "control lifecycle") {
		t.Fatalf("graceful early exit extraction=%+v", extraction)
	}
}

func TestExtractEvidenceRejectsWrongDirectionEvenWhenSUTCrashes(t *testing.T) {
	fixture := newExtractionFixture(t)
	var result run.Result
	readJSONFile(t, fixture.resultPath, &result)
	result.Config.Netem.ServerEgress.LossPercent = fixture.request.Config.LossPercent
	result.Netem.Pair = pairSpecForRegime(result.Config.Netem)
	result.Processes = result.Processes[:1]
	result.Processes[0].ExitCode = 9
	result.Processes[0].Error = "exit status 9"
	result.Control = nil
	result.Metrics = nil
	result.Verdict = run.VerdictInvalid
	result.InvalidReasons = []string{"control result is missing", "merged metrics are missing", "server proc_index=-1 pid=100 exit_code=9"}
	result.Outcome = run.OutcomeFail
	writeJSONFile(t, fixture.resultPath, result)
	fixture.request.CaseID = CaseLossTolerantClientLoss
	definition, _ := caseDefinition(fixture.request.CaseID)
	fixture.request.ExpectedCaseIdentity = CaseIdentity(fixture.request.ExpectedProbeIdentity, definition)
	expectedConfig := result.Config
	fixture.request.ExpectedRunConfig = &expectedConfig
	fixture.request.ExpectedRunIdentity = recordedRunIdentity(result)
	fixture.refreshAcquisitionID()
	writeJSONFile(t, fixture.resultPath, result)
	fixture.refreshResultHash(t)
	assertInvalidExtraction(t, fixture.request, "not client-egress-only")
}

func TestExtractEvidenceRejectsLatentCleanImpairmentOnCrash(t *testing.T) {
	fixture := newExtractionFixture(t)
	var result run.Result
	readJSONFile(t, fixture.resultPath, &result)
	result.Config.Netem.ClientEgress.LossSeed = 123
	result.Netem.Pair = pairSpecForRegime(result.Config.Netem)
	result.Processes = result.Processes[:1]
	result.Processes[0].ExitCode = 11
	result.Control = nil
	result.Metrics = nil
	result.Verdict = run.VerdictInvalid
	result.InvalidReasons = []string{"control result is missing", "merged metrics are missing", "server proc_index=-1 pid=100 exit_code=11"}
	result.Outcome = run.OutcomeInvalid
	expectedConfig := result.Config
	fixture.request.ExpectedRunConfig = &expectedConfig
	fixture.request.ExpectedRunIdentity = recordedRunIdentity(result)
	fixture.refreshAcquisitionID()
	writeJSONFile(t, fixture.resultPath, result)
	fixture.refreshResultHash(t)
	assertInvalidExtraction(t, fixture.request, "non-zero netem impairment")
}

func TestExtractEvidenceAcceptsRunnerLossCrashConsequences(t *testing.T) {
	fixture := newClientLossExtractionFixture(t)
	var result run.Result
	readJSONFile(t, fixture.resultPath, &result)
	result.Processes = result.Processes[:1]
	result.Processes[0].ExitCode = 12
	result.Processes[0].Error = "exit status 12"
	result.Control = nil
	result.Metrics = nil
	result.Netem.LossEvidence = nil
	result.Verdict = run.VerdictInvalid
	result.InvalidReasons = []string{
		"control result is missing",
		"merged metrics are missing",
		"netem loss evidence: missing for configured random loss",
		"server proc_index=-1 pid=100 exit_code=12: exit status 12",
	}
	result.Outcome = run.OutcomeInvalid
	fixture.request.ExpectedRunIdentity = recordedRunIdentity(result)
	fixture.refreshAcquisitionID()
	writeJSONFile(t, fixture.resultPath, result)
	fixture.refreshResultHash(t)

	extraction, err := ExtractEvidence(fixture.request)
	if err != nil {
		t.Fatal(err)
	}
	if len(extraction.InvalidReasons) != 0 || extraction.Evidence == nil ||
		!containsReason(extraction.Evidence.SUTFailureReasons, "exit_code=12") {
		t.Fatalf("runner-format loss crash extraction=%+v", extraction)
	}
}

func TestExtractEvidenceAcceptsRunnerClientUDPSUTFailure(t *testing.T) {
	fixture := newExtractionFixture(t)
	var result run.Result
	readJSONFile(t, fixture.resultPath, &result)
	result.Netem.UDPAfter.RcvbufErrors = 1
	result.Netem.UDPDelta.RcvbufErrors = 1
	result.Verdict = run.VerdictInvalid
	result.InvalidReasons = []string{"client netns UDP drop delta non-zero: InErrors=0 RcvbufErrors=1"}
	result.Outcome = run.OutcomeInvalid
	writeJSONFile(t, fixture.resultPath, result)
	fixture.refreshResultHash(t)

	extraction, err := ExtractEvidence(fixture.request)
	if err != nil {
		t.Fatal(err)
	}
	if len(extraction.InvalidReasons) != 0 || extraction.Evidence == nil ||
		!containsReason(extraction.Evidence.SUTFailureReasons, "client UDP receive drops") {
		t.Fatalf("runner-format client UDP failure extraction=%+v", extraction)
	}
}

func TestExtractEvidenceRejectsStoredUDPDeltaMismatch(t *testing.T) {
	fixture := newExtractionFixture(t)
	var result run.Result
	readJSONFile(t, fixture.resultPath, &result)
	result.Netem.UDPDelta.RcvbufErrors = 1
	result.Verdict = run.VerdictInvalid
	result.InvalidReasons = []string{"client netns UDP drop delta non-zero: InErrors=0 RcvbufErrors=1"}
	result.Outcome = run.OutcomeInvalid
	writeJSONFile(t, fixture.resultPath, result)
	fixture.refreshResultHash(t)
	assertInvalidExtraction(t, fixture.request, "stored UDP delta")
}

func TestExtractEvidenceDoesNotLetNetworkFailureHideMissingProcessIdentity(t *testing.T) {
	fixture := newExtractionFixture(t)
	var result run.Result
	readJSONFile(t, fixture.resultPath, &result)
	result.Processes = result.Processes[:1]
	result.Netem.UDPDelta.RcvbufErrors = 1
	result.Verdict = run.VerdictInvalid
	result.InvalidReasons = []string{"client netns UDP drop delta non-zero: InErrors=0 RcvbufErrors=1"}
	result.Outcome = run.OutcomeInvalid
	writeJSONFile(t, fixture.resultPath, result)
	fixture.refreshResultHash(t)
	assertInvalidExtraction(t, fixture.request, "process records")
}

func TestExtractEvidenceVerifiesDirectionalLossAndInnerWindow(t *testing.T) {
	fixture := newClientLossExtractionFixture(t)
	extraction, err := ExtractEvidence(fixture.request)
	if err != nil {
		t.Fatal(err)
	}
	if len(extraction.InvalidReasons) != 0 || extraction.Evidence == nil {
		t.Fatalf("loss extraction invalid: %+v", extraction)
	}
	evidence := extraction.Evidence
	if evidence.Loss.Mode != LossModeRandomNetem || evidence.Loss.ClientEgressDropped != 10 ||
		evidence.Loss.ServerEgressDropped != 0 || !isSHA256(evidence.LossEvidenceSHA256) {
		t.Fatalf("directional loss was not derived: %+v", evidence)
	}
	if evidence.EligibleDeliveredUnique == 0 || evidence.EligibleDeliveredUnique >= evidence.DeliveredUnique {
		t.Fatalf("inner-window delivery bound=%d, delivered=%d", evidence.EligibleDeliveredUnique, evidence.DeliveredUnique)
	}
}

func TestExtractEvidenceRejectsTamperedLossDelta(t *testing.T) {
	fixture := newClientLossExtractionFixture(t)
	var result run.Result
	readJSONFile(t, fixture.resultPath, &result)
	result.Netem.LossEvidence.Delta.ClientEgress.Dropped++
	writeJSONFile(t, fixture.resultPath, result)
	fixture.refreshResultHash(t)
	assertInvalidExtraction(t, fixture.request, "stored counter delta does not match snapshots")
}

func TestExtractEvidenceRejectsQdiscTrafficBelowEligiblePayload(t *testing.T) {
	fixture := newClientLossExtractionFixture(t)
	var result run.Result
	readJSONFile(t, fixture.resultPath, &result)
	clientAfter := *result.Netem.LossEvidence.After.ClientEgress.Stats
	clientAfter.SentBytes = result.Netem.LossEvidence.Before.ClientEgress.Stats.SentBytes + 1
	result.Netem.LossEvidence.After.ClientEgress.Stats = &clientAfter
	result.Netem.LossEvidence.After.ClientEgress.Raw = extractionQdiscRaw(clientAfter)
	delta := netops.DeltaQdiscPair(*result.Netem.LossEvidence.Before, *result.Netem.LossEvidence.After)
	result.Netem.LossEvidence.Delta = &delta
	writeJSONFile(t, fixture.resultPath, result)
	fixture.refreshResultHash(t)
	assertInvalidExtraction(t, fixture.request, "qdisc sent_bytes")
}

const extractionTransport = "fake"

type extractionFixture struct {
	request           ExtractRequest
	resultPath        string
	clientMetricsPath string
}

func newExtractionFixture(t *testing.T) extractionFixture {
	t.Helper()
	root := t.TempDir()
	metricsDir := filepath.Join(root, "metrics")
	if err := os.Mkdir(metricsDir, 0o755); err != nil {
		t.Fatal(err)
	}

	config := DefaultConfig(1)
	mapping := extractionMapping()
	selected := run.TrafficClassSpec{RateHz: float64(config.RateHz), PayloadBytes: int(config.PayloadBytes)}
	scenario := run.ScenarioSpec{
		Name: "class-mapping-loss-tolerant",
		Kind: run.ScenarioEnvironmentBaseline,
		ClientInput: &run.TrafficSpec{
			TrafficID:    run.TrafficIDClientInput,
			LossTolerant: selected,
		},
	}
	netem := &run.NetemRegime{
		Prefix:          "extract-test",
		ServerNS:        "extract-srv",
		ClientNS:        "extract-cli",
		ServerVeth:      "extract-vs",
		ClientVeth:      "extract-vc",
		ServerAddrCIDR:  "10.250.0.1/24",
		ClientAddrCIDR:  "10.250.0.2/24",
		LinkMTUBytes:    int(config.LinkMTUBytes),
		DisableOffloads: true,
	}
	runConfig := run.RunConfig{
		Transport:          extractionTransport,
		ClassMappingSHA256: mapping.ServerSHA256,
		Scenario:           &scenario,
		ServerCommand:      run.CommandConfig{Path: "/bin/true"},
		ClientCommand:      run.CommandConfig{Path: "/bin/true"},
		ClientProcs:        1,
		TotalConns:         1,
		Warmup:             run.Duration{Duration: time.Duration(config.WarmupNS)},
		Duration:           run.Duration{Duration: time.Duration(config.DurationNS)},
		Drain:              run.Duration{Duration: time.Duration(config.DrainNS)},
		StalenessPeriodNS:  10_000_000,
		Netem:              netem,
		AttemptedThreshold: 1,
		ControlTimeout:     run.Duration{Duration: 30 * time.Second},
		ProcessExitTimeout: run.Duration{Duration: 5 * time.Second},
		OutputDir:          root,
	}

	serverMetrics := filepath.Join(metricsDir, "server.json")
	clientMetrics := filepath.Join(metricsDir, "client-0.json")
	writeMetricFixture(t, serverMetrics, "", 0, 0)
	writeMetricFixture(t, clientMetrics, run.ClassLossTolerant, config.SlotsPerRun, config.SlotsPerRun)
	merged, err := run.MergeMetricsFiles([]string{clientMetrics}, 1)
	if err != nil {
		t.Fatal(err)
	}

	description := extractionDescription(t, mapping.Server)
	offloads := extractionOffloadEvidence(*netem)
	start := int64(10 * time.Second)
	schedule := control.ScheduleMessage{
		Type:         control.TypeSchedule,
		StartAtNS:    start,
		StopAtNS:     start + int64(runConfig.Duration.Duration),
		DrainUntilNS: start + int64(runConfig.Duration.Duration+runConfig.Drain.Duration),
	}
	participant := func(role string, index, pid int) control.Participant {
		connID := 0
		readyConns := 0
		if role == "client" {
			connID = 1
			readyConns = 1
		}
		return control.Participant{
			ConnID: connID,
			Hello:  control.HelloMessage{Type: control.TypeHello, Role: role, Transport: extractionTransport, PID: pid, ProcIndex: index},
			Ready:  control.ReadyMessage{Type: control.TypeReady, Conns: readyConns}, ReadyReceived: true,
			SchedAck: control.SchedAckMessage{Type: control.TypeSchedAck, MarginNS: 1_000_000}, AckReceived: true,
			Done: control.DoneMessage{Type: control.TypeDone, Stats: json.RawMessage(`{"invalid_payload":0}`)}, DoneReceived: true,
		}
	}
	result := run.Result{
		Version:        resultVersion,
		Transport:      extractionTransport,
		Outcome:        run.OutcomeInconclusive,
		OutcomeReasons: []string{"primary SLOs missing"},
		Verdict:        run.VerdictValid,
		Config:         runConfig,
		Control: &control.Result{Valid: true, Schedule: schedule, Participants: []control.Participant{
			participant("server", 0, 100), participant("client", 0, 101),
		}},
		Processes: []run.ProcessResult{
			{Role: "server", ProcIndex: -1, PID: 100, Command: extractionProcessCommand(netem.ServerNS), MetricsOut: serverMetrics, Exited: true},
			{Role: "client", ProcIndex: 0, PID: 101, Conns: 1, OriginIDEnd: 1, Command: extractionProcessCommand(netem.ClientNS), MetricsOut: clientMetrics, Exited: true},
		},
		Metrics: merged,
		Treatment: &run.TreatmentRecord{
			OrchestratorSHA256: strings.Repeat("a", 64),
			EnvironmentSHA256:  strings.Repeat("b", 64),
			Server:             run.CommandDescription{SHA256: strings.Repeat("c", 64), Description: description},
			Client:             run.CommandDescription{SHA256: strings.Repeat("c", 64), Description: description},
			ClassMapping:       mapping,
		},
		Netem: &run.NetemResult{
			Enabled: true, Pair: pairSpecForRegime(netem), Offloads: &offloads, OffloadsAfter: &offloads,
			Gate: &netops.NetemGateReport{LossC2SPackets: 2000, LossS2CPackets: 2000},
		},
	}
	resultPath := filepath.Join(root, "result.json")
	writeJSONFile(t, resultPath, result)
	resultData, err := os.ReadFile(resultPath)
	if err != nil {
		t.Fatal(err)
	}
	mappingSHA := MappingSHA256(mapping)
	probeIdentity := ProbeIdentity(config, extractionTransport, mappingSHA)
	definition, _ := caseDefinition(CaseCleanLossTolerant)
	runIdentity := recordedRunIdentity(result)
	caseIdentity := CaseIdentity(probeIdentity, definition)
	request := ExtractRequest{
		RunDir:                root,
		CaseID:                definition.ID,
		AttemptNumber:         1,
		AcquisitionID:         AcquisitionIdentity(caseIdentity, 1, runIdentity),
		Config:                config,
		Mapping:               mapping,
		ExpectedTransport:     extractionTransport,
		ExpectedRunConfig:     &runConfig,
		ExpectedRunIdentity:   runIdentity,
		ExpectedResultSHA256:  run.HashBytes(resultData),
		ExpectedProbeIdentity: probeIdentity,
		ExpectedCaseIdentity:  caseIdentity,
	}
	return extractionFixture{request: request, resultPath: resultPath, clientMetricsPath: clientMetrics}
}

func extractionProcessCommand(namespace string) []string {
	return []string{"ip", "netns", "exec", namespace, "setpriv", "--reuid", "1000", "--regid", "1000", "--init-groups", "/bin/true"}
}

func newClientLossExtractionFixture(t *testing.T) extractionFixture {
	t.Helper()
	fixture := newExtractionFixture(t)
	var result run.Result
	readJSONFile(t, fixture.resultPath, &result)
	result.Config.Netem.ClientEgress.LossPercent = fixture.request.Config.LossPercent
	result.Netem.Pair = pairSpecForRegime(result.Config.Netem)
	result.Netem.Gate.ExpectedC2SPct = fixture.request.Config.LossPercent
	result.Netem.Gate.LossC2SPct = fixture.request.Config.LossPercent
	writeMetricFixture(t, fixture.clientMetricsPath, run.ClassLossTolerant,
		fixture.request.Config.SlotsPerRun, fixture.request.Config.SlotsPerRun-10)
	merged, err := run.MergeMetricsFiles([]string{fixture.clientMetricsPath}, 1)
	if err != nil {
		t.Fatal(err)
	}
	result.Metrics = merged
	result.Netem.LossEvidence = extractionLossEvidence(result)
	result.Outcome = run.OutcomeInconclusive
	result.Verdict = run.VerdictValid
	result.InvalidReasons = nil
	result.OutcomeReasons = []string{"primary SLOs missing"}

	fixture.request.CaseID = CaseLossTolerantClientLoss
	definition, _ := caseDefinition(fixture.request.CaseID)
	fixture.request.ExpectedCaseIdentity = CaseIdentity(fixture.request.ExpectedProbeIdentity, definition)
	expectedConfig := result.Config
	fixture.request.ExpectedRunConfig = &expectedConfig
	fixture.request.ExpectedRunIdentity = recordedRunIdentity(result)
	fixture.refreshAcquisitionID()
	writeJSONFile(t, fixture.resultPath, result)
	fixture.refreshResultHash(t)
	return fixture
}

func extractionLossEvidence(result run.Result) *run.NetemLossEvidence {
	schedule := result.Control.Schedule
	serverBefore := netops.QdiscStats{Kind: "noqueue", Root: true, SentBytes: 1_000, SentPackets: 100}
	serverAfter := serverBefore
	serverAfter.SentBytes = 101_000
	serverAfter.SentPackets = 1_100
	clientBefore := netops.QdiscStats{Kind: "netem", Root: true, Limit: 10_000,
		LossPercent: result.Config.Netem.ClientEgress.LossPercent, SentBytes: 1_000, SentPackets: 100}
	clientAfter := clientBefore
	clientAfter.SentBytes = 1_001_000
	clientAfter.SentPackets = 1_090
	clientAfter.Dropped = 10
	sample := func(namespace, device string, start, finish int64, stats netops.QdiscStats) netops.QdiscSample {
		return netops.QdiscSample{Namespace: namespace, Device: device, CaptureStartNS: start,
			CaptureFinishNS: finish, Stats: &stats, Raw: extractionQdiscRaw(stats)}
	}
	before := netops.QdiscPairSnapshot{
		CaptureStartNS:  schedule.StartAtNS + int64(time.Millisecond),
		CaptureFinishNS: schedule.StartAtNS + 4*int64(time.Millisecond),
		ServerEgress: sample(result.Config.Netem.ServerNS, result.Config.Netem.ServerVeth,
			schedule.StartAtNS+int64(time.Millisecond), schedule.StartAtNS+2*int64(time.Millisecond), serverBefore),
		ClientEgress: sample(result.Config.Netem.ClientNS, result.Config.Netem.ClientVeth,
			schedule.StartAtNS+2*int64(time.Millisecond), schedule.StartAtNS+3*int64(time.Millisecond), clientBefore),
	}
	after := netops.QdiscPairSnapshot{
		CaptureStartNS:  schedule.StopAtNS - 4*int64(time.Millisecond),
		CaptureFinishNS: schedule.StopAtNS - int64(time.Millisecond),
		ServerEgress: sample(result.Config.Netem.ServerNS, result.Config.Netem.ServerVeth,
			schedule.StopAtNS-4*int64(time.Millisecond), schedule.StopAtNS-3*int64(time.Millisecond), serverAfter),
		ClientEgress: sample(result.Config.Netem.ClientNS, result.Config.Netem.ClientVeth,
			schedule.StopAtNS-3*int64(time.Millisecond), schedule.StopAtNS-2*int64(time.Millisecond), clientAfter),
	}
	delta := netops.DeltaQdiscPair(before, after)
	return &run.NetemLossEvidence{
		Version: 1, Mode: string(LossModeRandomNetem), Supported: true,
		Scope: LossEvidenceScopeEffectiveInner, Schedule: schedule, Before: &before, After: &after, Delta: &delta,
	}
}

func extractionQdiscRaw(stats netops.QdiscStats) string {
	line := fmt.Sprintf("qdisc %s 1: root", stats.Kind)
	if stats.Kind == "netem" {
		line += fmt.Sprintf(" limit %d loss random %g%%", stats.Limit, stats.LossPercent)
	}
	return fmt.Sprintf("%s\nSent %d bytes %d pkt (dropped %d, overlimits %d requeues %d)\n",
		line, stats.SentBytes, stats.SentPackets, stats.Dropped, stats.Overlimits, stats.Requeues)
}

func extractionMapping() run.ClassMappingRecord {
	mapping := run.ClassMappingRecord{
		Server: map[string]run.ClassMappingSpec{
			run.ClassLossTolerant: {Primitive: "datagram", Delivery: run.ClassMappingDeliveryBestEffort, Ordering: run.ClassMappingOrderingUnordered, Realization: run.ClassMappingRealizationNative},
			run.ClassMustDeliver:  {Primitive: "stream", Delivery: run.ClassMappingDeliveryReliable, Ordering: run.ClassMappingOrderingOrdered, Realization: run.ClassMappingRealizationNative},
		},
		Client: map[string]run.ClassMappingSpec{
			run.ClassLossTolerant: {Primitive: "datagram", Delivery: run.ClassMappingDeliveryBestEffort, Ordering: run.ClassMappingOrderingUnordered, Realization: run.ClassMappingRealizationNative},
			run.ClassMustDeliver:  {Primitive: "stream", Delivery: run.ClassMappingDeliveryReliable, Ordering: run.ClassMappingOrderingOrdered, Realization: run.ClassMappingRealizationNative},
		},
		Match: true,
	}
	mapping.ServerSHA256 = run.HashValue(mapping.Server)
	mapping.ClientSHA256 = run.HashValue(mapping.Client)
	return mapping
}

func extractionDescription(t *testing.T, mapping map[string]run.ClassMappingSpec) json.RawMessage {
	t.Helper()
	data, err := json.Marshal(map[string]any{
		"transport": extractionTransport, "class_mapping": mapping, "coalescing": "none",
		"cc_algo": "none", "thread_model": "single", "encryption": false,
		"payload_pattern": "splitmix64-v1", "wire_compression": "none",
		"max_payload_bytes": 1500, "scenarios": []string{string(run.ScenarioEnvironmentBaseline)},
		"tuning": []any{},
	})
	if err != nil {
		t.Fatal(err)
	}
	return data
}

func extractionOffloadEvidence(regime run.NetemRegime) netops.OffloadEvidence {
	features := map[string]netops.OffloadFeatureState{}
	var raw strings.Builder
	raw.WriteString("Features for benchmark-veth:\n")
	for _, name := range netops.RequiredOffloadFeatures() {
		features[name] = netops.OffloadFeatureState{}
		raw.WriteString(name)
		raw.WriteString(": off\n")
	}
	evidence := netops.OffloadEvidence{
		Version:          netops.OffloadEvidenceVersion,
		RequiredFeatures: netops.RequiredOffloadFeatures(),
		Server: netops.OffloadInterfaceEvidence{Namespace: regime.ServerNS, Device: regime.ServerVeth,
			LinkMTUBytes: regime.LinkMTUBytes, LinkRaw: `[{"ifname":"` + regime.ServerVeth + `","mtu":1500}]`,
			Features: features, Raw: raw.String()},
		Client: netops.OffloadInterfaceEvidence{Namespace: regime.ClientNS, Device: regime.ClientVeth,
			LinkMTUBytes: regime.LinkMTUBytes, LinkRaw: `[{"ifname":"` + regime.ClientVeth + `","mtu":1500}]`,
			Features: features, Raw: raw.String()},
	}
	evidence.SHA256 = netops.HashOffloadEvidence(evidence)
	return evidence
}

type metricClassFixture struct {
	run.ClassCounts
	LatencySchedNS run.Histogram `json:"latency_sched_ns"`
	LatencySendNS  run.Histogram `json:"latency_send_ns"`
	UpdateGapNS    run.Histogram `json:"update_gap_ns"`
}

type trafficMetricFixture struct {
	TrafficID  uint8  `json:"traffic_id"`
	Direction  string `json:"direction"`
	Class      string `json:"class"`
	DeadlineNS uint64 `json:"deadline_ns"`
	run.ClassCounts
	LatencySchedNS run.Histogram `json:"latency_sched_ns"`
	LatencySendNS  run.Histogram `json:"latency_send_ns"`
	UpdateGapNS    run.Histogram `json:"update_gap_ns"`
	StalenessNS    run.Histogram `json:"staleness_ns"`
}

func writeMetricFixture(t *testing.T, path, class string, slots, delivered uint64) {
	t.Helper()
	writeMetricFixtureCounts(t, path, class, slots, slots, delivered)
}

func writeMetricFixtureCounts(t *testing.T, path, class string, slots, submitted, delivered uint64) {
	t.Helper()
	layout := run.HistogramLayout{Scheme: "log2x16", Subbins: 16, MinNS: 1_000, MaxNS: 100_000_000_000}
	empty := fixtureHistogram(0)
	classes := map[string]metricClassFixture{
		run.ClassLossTolerant: {LatencySchedNS: empty, LatencySendNS: empty, UpdateGapNS: empty},
		run.ClassMustDeliver:  {LatencySchedNS: empty, LatencySendNS: empty, UpdateGapNS: empty},
	}
	traffic := make([]trafficMetricFixture, 0, 1)
	staleness := empty
	if class != "" {
		counts := run.ClassCounts{Slots: slots, Submitted: submitted, DeliveredUnique: delivered}
		if class == run.ClassLossTolerant {
			counts.ExpectedFlows = 1
			counts.ObservedFlows = 1
			staleness = fixtureHistogram(2000)
		}
		latency := fixtureHistogram(delivered)
		update := fixtureHistogram(delivered)
		classes[class] = metricClassFixture{ClassCounts: counts, LatencySchedNS: latency, LatencySendNS: latency, UpdateGapNS: update}
		traffic = append(traffic, trafficMetricFixture{
			TrafficID: run.TrafficIDClientInput, Direction: run.DirectionRoomRelay, Class: class,
			ClassCounts: counts, LatencySchedNS: latency, LatencySendNS: latency, UpdateGapNS: update,
			StalenessNS: staleness,
		})
	}
	file := struct {
		Version     int                           `json:"version"`
		Histogram   run.HistogramLayout           `json:"histogram"`
		Classes     map[string]metricClassFixture `json:"classes"`
		StalenessNS run.Histogram                 `json:"staleness_ns"`
		Raw         run.RawCounts                 `json:"raw"`
		Traffic     []trafficMetricFixture        `json:"traffic"`
	}{
		Version: 2, Histogram: layout, Classes: classes, StalenessNS: staleness,
		Raw: run.RawCounts{Slots: slots, Submitted: submitted, RecvMeasured: delivered}, Traffic: traffic,
	}
	writeJSONFile(t, path, file)
}

func fixtureHistogram(count uint64) run.Histogram {
	bins := make([]uint64, 448)
	bins[0] = count
	return run.Histogram{
		Scheme: "log2x16", MinNS: 1_000, MaxNS: 100_000_000_000,
		Count: count, P50NS: 1_000, P90NS: 1_000, P99NS: 1_000, Bins: bins,
	}
}

func (fixture *extractionFixture) refreshResultHash(t *testing.T) {
	t.Helper()
	data, err := os.ReadFile(fixture.resultPath)
	if err != nil {
		t.Fatal(err)
	}
	fixture.request.ExpectedResultSHA256 = run.HashBytes(data)
}

func (fixture *extractionFixture) refreshAcquisitionID() {
	fixture.request.AcquisitionID = AcquisitionIdentity(
		fixture.request.ExpectedCaseIdentity,
		fixture.request.AttemptNumber,
		fixture.request.ExpectedRunIdentity,
	)
}

func assertInvalidExtraction(t *testing.T, request ExtractRequest, fragment string) {
	t.Helper()
	extraction, err := ExtractEvidence(request)
	if err != nil {
		t.Fatal(err)
	}
	if extraction.Evidence != nil || !containsReason(extraction.InvalidReasons, fragment) {
		t.Fatalf("extraction=%+v, want invalid reason containing %q", extraction, fragment)
	}
}

func containsReason(reasons []string, fragment string) bool {
	for _, reason := range reasons {
		if strings.Contains(reason, fragment) {
			return true
		}
	}
	return false
}

func readJSONFile(t *testing.T, path string, target any) {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if err := json.Unmarshal(data, target); err != nil {
		t.Fatal(err)
	}
}

func writeJSONFile(t *testing.T, path string, value any) {
	t.Helper()
	data, err := json.Marshal(value)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}
}
