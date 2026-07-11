package conformance

import (
	"encoding/json"
	"fmt"
	"math"
	"strings"
	"testing"

	"github.com/neguse/rudp-bench/orchestrator/run"
)

const testTransport = "test-transport"

func TestEvaluatePassesCompleteBehavioralMatrix(t *testing.T) {
	cfg := DefaultConfig(1)
	mapping := validMapping()
	evidence := passingEvidence(cfg, mapping)

	report := Evaluate(cfg, testTransport, mapping, evidence)
	if report.Outcome != run.OutcomePass {
		t.Fatalf("outcome=%s reasons=%v", report.Outcome, report.Reasons)
	}
	if report.MappingSHA256 != MappingSHA256(mapping) || !isSHA256(report.EvidenceSHA256) {
		t.Fatalf("hashes were not preserved: %+v", report)
	}
	if !report.Observed.Delivery || report.Observed.Ordering || report.Observed.Primitive || report.Observed.Realization {
		t.Fatalf("observation scope overclaims: %+v", report.Observed)
	}
	if len(report.Observed.Limitations) != 5 || !reasonsContain(report.Observed.Limitations, "fixed drain_ns") {
		t.Fatalf("limitations=%v", report.Observed.Limitations)
	}
	for _, item := range report.Cases {
		if item.Outcome != run.OutcomePass {
			t.Fatalf("case %s outcome=%s reasons=%v", item.ID, item.Outcome, item.Reasons)
		}
		if !item.Clean && (item.PZeroUpperBound > item.Alpha || item.Alpha != cfg.CaseAlpha()) {
			t.Fatalf("case %s power p_zero_upper_bound=%g alpha=%g", item.ID, item.PZeroUpperBound, item.Alpha)
		}
		if !item.Clean {
			wantTrials := targetPacketTrials(item.EligibleDeliveredUnique, cfg.PayloadBytes, cfg.LinkMTUBytes)
			if item.Trials != wantTrials {
				t.Fatalf("case %s target packet trials=%d, want conservative lower bound %d", item.ID, item.Trials, wantTrials)
			}
		}
	}
}

func TestReliableBehaviorDeclaredBestEffortRemainsInconclusive(t *testing.T) {
	cfg := DefaultConfig(1)
	mapping := validMapping()
	evidence := passingEvidence(cfg, mapping)
	for index := range evidence {
		if evidence[index].CaseID == CaseLossTolerantClientLoss || evidence[index].CaseID == CaseLossTolerantServerLoss {
			evidence[index].DeliveredUnique = evidence[index].ExpectedReceives
		}
	}

	report := Evaluate(cfg, testTransport, mapping, evidence)
	if report.Outcome != run.OutcomeInconclusive {
		t.Fatalf("best_effort declaration without observed loss outcome=%s reasons=%v", report.Outcome, report.Reasons)
	}
	for _, id := range []ProbeCaseID{CaseLossTolerantClientLoss, CaseLossTolerantServerLoss} {
		item := findCase(t, report, id)
		if item.Outcome != run.OutcomeInconclusive || !reasonsContain(item.Reasons, "no application loss") {
			t.Fatalf("case %s overclaimed best_effort behavior: %+v", id, item)
		}
	}
}

func TestMutationBestEffortBehaviorFalselyDeclaredReliableFails(t *testing.T) {
	cfg := DefaultConfig(1)
	mapping := validMapping()
	for _, endpoint := range []map[string]run.ClassMappingSpec{mapping.Server, mapping.Client} {
		spec := endpoint[run.ClassLossTolerant]
		spec.Delivery = run.ClassMappingDeliveryReliable
		spec.Ordering = run.ClassMappingOrderingOrdered
		spec.Realization = run.ClassMappingRealizationReliableFallback
		endpoint[run.ClassLossTolerant] = spec
	}
	refreshMappingHashes(&mapping)
	evidence := passingEvidence(cfg, mapping)
	for index := range evidence {
		if evidence[index].CaseID == CaseLossTolerantClientLoss || evidence[index].CaseID == CaseLossTolerantServerLoss {
			evidence[index].DeliveredUnique = evidence[index].ExpectedReceives - 10
		}
	}

	report := Evaluate(cfg, testTransport, mapping, evidence)
	if report.Outcome != run.OutcomeFail {
		t.Fatalf("lying reliable declaration outcome=%s reasons=%v", report.Outcome, report.Reasons)
	}
	for _, id := range []ProbeCaseID{CaseLossTolerantClientLoss, CaseLossTolerantServerLoss} {
		item := findCase(t, report, id)
		if item.Outcome != run.OutcomeFail || !reasonsContain(item.Reasons, "declared reliable") {
			t.Fatalf("case %s did not catch mutation: %+v", id, item)
		}
	}
}

func TestEvaluateKeepsClientAndServerLossEvidenceSeparate(t *testing.T) {
	cfg := DefaultConfig(1)
	mapping := validMapping()
	evidence := passingEvidence(cfg, mapping)
	for index := range evidence {
		if evidence[index].CaseID == CaseLossTolerantClientLoss {
			evidence[index].DeliveredUnique = evidence[index].ExpectedReceives
		}
	}

	report := Evaluate(cfg, testTransport, mapping, evidence)
	client := findCase(t, report, CaseLossTolerantClientLoss)
	server := findCase(t, report, CaseLossTolerantServerLoss)
	if client.Outcome != run.OutcomeInconclusive || server.Outcome != run.OutcomePass {
		t.Fatalf("client/server attribution client=%+v server=%+v", client, server)
	}
}

func TestMutationOfOneEndpointDeclarationIsDetectedAndLocalized(t *testing.T) {
	cfg := DefaultConfig(1)
	mapping := validMapping()
	clientLT := mapping.Client[run.ClassLossTolerant]
	clientLT.Delivery = run.ClassMappingDeliveryReliable
	clientLT.Ordering = run.ClassMappingOrderingOrdered
	clientLT.Realization = run.ClassMappingRealizationReliableFallback
	mapping.Client[run.ClassLossTolerant] = clientLT
	refreshMappingHashes(&mapping)
	evidence := passingEvidence(cfg, mapping)

	report := Evaluate(cfg, testTransport, mapping, evidence)
	if report.Outcome != run.OutcomeInvalid || !reasonsContain(report.Reasons, "mismatch") {
		t.Fatalf("one-endpoint mutation was not rejected: outcome=%s reasons=%v", report.Outcome, report.Reasons)
	}
	client := findCase(t, report, CaseLossTolerantClientLoss)
	server := findCase(t, report, CaseLossTolerantServerLoss)
	if client.Outcome != run.OutcomeFail || server.Outcome != run.OutcomeInconclusive ||
		!reasonsContain(server.Reasons, "clean control") {
		t.Fatalf("mutation was not localized: client=%+v server=%+v", client, server)
	}
}

func TestEvaluateInsufficientBestEffortExposureIsInconclusive(t *testing.T) {
	cfg := DefaultConfig(1)
	setSlots(&cfg, 10)
	mapping := validMapping()
	evidence := passingEvidence(cfg, mapping)
	for index := range evidence {
		if evidence[index].CaseID == CaseLossTolerantClientLoss {
			evidence[index].DeliveredUnique = 10
		}
	}

	report := Evaluate(cfg, testTransport, mapping, evidence)
	item := findCase(t, report, CaseLossTolerantClientLoss)
	if item.Outcome != run.OutcomeInconclusive || item.PZeroUpperBound <= item.Alpha {
		t.Fatalf("case=%+v", item)
	}
	if report.Outcome != run.OutcomeInconclusive {
		t.Fatalf("overall outcome=%s reasons=%v", report.Outcome, report.Reasons)
	}
}

func TestObservedBestEffortLossPassesWithoutStatisticalExtrapolation(t *testing.T) {
	cfg := DefaultConfig(1)
	setSlots(&cfg, 10)
	mapping := validMapping()
	evidence := passingEvidence(cfg, mapping)
	item := evidenceByID(t, evidence, CaseLossTolerantClientLoss)
	item.DeliveredUnique = 9
	item.EligibleDeliveredUnique = 9

	report := Evaluate(cfg, testTransport, mapping, evidence)
	caseReport := findCase(t, report, CaseLossTolerantClientLoss)
	if caseReport.Outcome != run.OutcomePass || caseReport.PZeroUpperBound <= caseReport.Alpha {
		t.Fatalf("direct best_effort observation should pass despite low power: %+v", caseReport)
	}
}

func TestEvaluateReliableRecoveryRequiresExposureAndIntegrity(t *testing.T) {
	cfg := DefaultConfig(1)
	mapping := validMapping()
	for name, testCase := range map[string]struct {
		mutate     func(*EvidenceRun)
		wantReason string
	}{
		"eventual missing": {func(item *EvidenceRun) { item.DeliveredUnique--; item.EligibleDeliveredUnique-- }, "delivered_by_drain"},
		"duplicate":        {func(item *EvidenceRun) { item.Duplicates = 1 }, "duplicates"},
		"corruption":       {func(item *EvidenceRun) { item.Corruption = 1 }, "corruption"},
	} {
		t.Run(name, func(t *testing.T) {
			evidence := passingEvidence(cfg, mapping)
			item := evidenceByID(t, evidence, CaseMustDeliverClientLoss)
			testCase.mutate(item)
			report := Evaluate(cfg, testTransport, mapping, evidence)
			caseReport := findCase(t, report, CaseMustDeliverClientLoss)
			if caseReport.Outcome != run.OutcomeFail || !reasonsContain(caseReport.Reasons, testCase.wantReason) {
				t.Fatalf("case=%+v", caseReport)
			}
		})
	}
}

func TestEvaluateSUTFailureAndDirectCorruptionAreFailures(t *testing.T) {
	cfg := DefaultConfig(1)
	mapping := validMapping()

	evidence := passingEvidence(cfg, mapping)
	crashed := evidenceByID(t, evidence, CaseMustDeliverClientLoss)
	crashed.SUTFailureReasons = []string{"client process crashed"}
	crashed.Slots = 0
	crashed.Submitted = 0
	crashed.ExpectedReceives = 0
	crashed.DeliveredUnique = 0
	crashed.EligibleDeliveredUnique = 0
	crashed.Loss.Supported = false
	crashed.Loss.Scope = ""
	crashed.Loss.ClientEgressDropped = 0
	crashed.Loss.ServerEgressDropped = 0
	crashed.Loss.QueueOverflowDrops = 0
	crashed.LossEvidenceSHA256 = ""
	report := Evaluate(cfg, testTransport, mapping, evidence)
	caseReport := findCase(t, report, CaseMustDeliverClientLoss)
	if caseReport.Outcome != run.OutcomeFail || !reasonsContain(caseReport.Reasons, "process crashed") {
		t.Fatalf("crash case=%+v", caseReport)
	}

	evidence = passingEvidence(cfg, mapping)
	corrupt := evidenceByID(t, evidence, CaseMustDeliverClientLoss)
	corrupt.Submitted--
	corrupt.DeliveredUnique--
	corrupt.EligibleDeliveredUnique--
	corrupt.Corruption = 1
	report = Evaluate(cfg, testTransport, mapping, evidence)
	caseReport = findCase(t, report, CaseMustDeliverClientLoss)
	if caseReport.Outcome != run.OutcomeFail || !reasonsContain(caseReport.Reasons, "corruption") {
		t.Fatalf("incomplete+corrupt case=%+v", caseReport)
	}
}

func TestEvaluateCleanBestEffortDuplicateFails(t *testing.T) {
	cfg := DefaultConfig(1)
	mapping := validMapping()
	evidence := passingEvidence(cfg, mapping)
	item := evidenceByID(t, evidence, CaseCleanLossTolerant)
	item.Submitted--
	item.DeliveredUnique--
	item.EligibleDeliveredUnique--
	item.Duplicates = 1

	report := Evaluate(cfg, testTransport, mapping, evidence)
	caseReport := findCase(t, report, CaseCleanLossTolerant)
	if caseReport.Outcome != run.OutcomeFail || !reasonsContain(caseReport.Reasons, "duplicates") {
		t.Fatalf("case=%+v", caseReport)
	}
}

func TestEvaluateCleanCompleteLossFails(t *testing.T) {
	cfg := DefaultConfig(1)
	mapping := validMapping()
	evidence := passingEvidence(cfg, mapping)
	item := evidenceByID(t, evidence, CaseCleanLossTolerant)
	item.DeliveredUnique = 0
	item.EligibleDeliveredUnique = 0

	report := Evaluate(cfg, testTransport, mapping, evidence)
	caseReport := findCase(t, report, CaseCleanLossTolerant)
	if caseReport.Outcome != run.OutcomeFail || !reasonsContain(caseReport.Reasons, "missing=1000") {
		t.Fatalf("case=%+v", caseReport)
	}
}

func TestEvaluateRejectsInvalidOneWayRandomLossEvidence(t *testing.T) {
	cfg := DefaultConfig(1)
	mapping := validMapping()
	for name, testCase := range map[string]struct {
		mutate     func(*LossContractEvidence)
		wantReason string
	}{
		"both directions": {func(loss *LossContractEvidence) { loss.ServerEgressLossPercent = cfg.LossPercent }, "not client-egress-only"},
		"seeded":          {func(loss *LossContractEvidence) { loss.LossSeed = 9 }, "loss_seed"},
		"burst":           {func(loss *LossContractEvidence) { loss.LossBurstLength = 4 }, "loss_burst_length"},
		"unsupported":     {func(loss *LossContractEvidence) { loss.Supported = false }, "not supported"},
		"wrong scope":     {func(loss *LossContractEvidence) { loss.Scope = "whole_run" }, "scope"},
		"other treatment": {func(loss *LossContractEvidence) { loss.RandomLossOnly = false }, "non-random"},
		"queue overflow":  {func(loss *LossContractEvidence) { loss.QueueOverflowDrops = 1 }, "queue_overflow"},
		"zero drops":      {func(loss *LossContractEvidence) { loss.ClientEgressDropped = 0 }, "zero observed"},
		"wrong direction": {func(loss *LossContractEvidence) { loss.ServerEgressDropped = 1 }, "unconfigured server"},
		"low qdisc bytes": {func(loss *LossContractEvidence) { loss.ClientEgressSentBytes = 1 }, "sent_bytes"},
		"low qdisc packets": {func(loss *LossContractEvidence) {
			loss.ClientEgressSentPackets = 1
		}, "sent_packets"},
	} {
		t.Run(name, func(t *testing.T) {
			evidence := passingEvidence(cfg, mapping)
			item := evidenceByID(t, evidence, CaseLossTolerantClientLoss)
			testCase.mutate(&item.Loss)
			report := Evaluate(cfg, testTransport, mapping, evidence)
			caseReport := findCase(t, report, CaseLossTolerantClientLoss)
			if caseReport.Outcome != run.OutcomeInvalid || !reasonsContain(caseReport.Reasons, testCase.wantReason) {
				t.Fatalf("case=%+v", caseReport)
			}
			if report.Outcome != run.OutcomeInvalid {
				t.Fatalf("overall outcome=%s", report.Outcome)
			}
		})
	}
}

func TestEvaluateRejectsProbeShapeAndCaseRelabeling(t *testing.T) {
	cfg := DefaultConfig(1)
	mapping := validMapping()
	for name, testCase := range map[string]struct {
		mutate     func(*EvidenceRun)
		wantReason string
	}{
		"scenario":        {func(item *EvidenceRun) { item.Scenario = run.ScenarioRoomRelay }, "scenario"},
		"class":           {func(item *EvidenceRun) { item.TrafficClass = run.ClassMustDeliver }, "traffic_class"},
		"mixed classes":   {func(item *EvidenceRun) { item.ClassExclusive = false }, "class-exclusive"},
		"not echo":        {func(item *EvidenceRun) { item.Echo = false }, "not echo"},
		"payload pattern": {func(item *EvidenceRun) { item.PayloadPatternVerified = false }, "payload pattern"},
		"compression":     {func(item *EvidenceRun) { item.WireCompressionDisabled = false }, "wire compression"},
		"coalescing":      {func(item *EvidenceRun) { item.ClientCoalescing = "" }, "coalescing disclosure"},
		"offloads":        {func(item *EvidenceRun) { item.OffloadsDisabled = false }, "offloads"},
		"case relabeled":  {func(item *EvidenceRun) { item.CaseID = CaseLossTolerantServerLoss }, "case_identity"},
	} {
		t.Run(name, func(t *testing.T) {
			evidence := passingEvidence(cfg, mapping)
			item := evidenceByID(t, evidence, CaseLossTolerantClientLoss)
			testCase.mutate(item)
			report := Evaluate(cfg, testTransport, mapping, evidence)
			if report.Outcome != run.OutcomeInvalid || !reasonsContain(report.Reasons, testCase.wantReason) {
				t.Fatalf("outcome=%s reasons=%v", report.Outcome, report.Reasons)
			}
		})
	}
}

func TestEvaluateRejectsHashAndMeasurementCorruption(t *testing.T) {
	cfg := DefaultConfig(1)
	mapping := validMapping()
	evidence := passingEvidence(cfg, mapping)
	item := evidenceByID(t, evidence, CaseLossTolerantClientLoss)
	item.MappingSHA256 = hashOf("different mapping")
	item.EndpointMappingSHA256 = hashOf("different endpoint mapping")
	item.MeasurementValid = false
	item.InvalidReasons = []string{"metrics aggregate mismatch"}

	report := Evaluate(cfg, testTransport, mapping, evidence)
	caseReport := findCase(t, report, CaseLossTolerantClientLoss)
	if caseReport.Outcome != run.OutcomeInvalid || !reasonsContain(caseReport.Reasons, "does not match") ||
		!reasonsContain(caseReport.Reasons, "endpoint_mapping_sha256") ||
		!reasonsContain(caseReport.Reasons, "metrics aggregate mismatch") {
		t.Fatalf("case=%+v", caseReport)
	}
}

func TestEvaluateIncompleteSubmissionAndMissingCaseAreInconclusive(t *testing.T) {
	cfg := DefaultConfig(1)
	mapping := validMapping()
	evidence := passingEvidence(cfg, mapping)
	item := evidenceByID(t, evidence, CaseLossTolerantClientLoss)
	item.Submitted--

	report := Evaluate(cfg, testTransport, mapping, evidence)
	caseReport := findCase(t, report, CaseLossTolerantClientLoss)
	if caseReport.Outcome != run.OutcomeInconclusive || !reasonsContain(caseReport.Reasons, "incomplete") {
		t.Fatalf("case=%+v", caseReport)
	}

	var withoutServerMD []EvidenceRun
	for _, item := range passingEvidence(cfg, mapping) {
		if item.CaseID != CaseMustDeliverServerLoss {
			withoutServerMD = append(withoutServerMD, item)
		}
	}
	report = Evaluate(cfg, testTransport, mapping, withoutServerMD)
	caseReport = findCase(t, report, CaseMustDeliverServerLoss)
	if caseReport.Outcome != run.OutcomeInconclusive || !reasonsContain(caseReport.Reasons, "not acquired") {
		t.Fatalf("case=%+v", caseReport)
	}
}

func TestEvaluateUnsupportedMapping(t *testing.T) {
	cfg := DefaultConfig(1)
	mapping := validMapping()
	unsupported := run.ClassMappingSpec{
		Primitive: "none", Delivery: run.ClassMappingDeliveryReliable,
		Ordering: run.ClassMappingOrderingOrdered, Realization: run.ClassMappingRealizationUnsupported,
	}
	mapping.Server[run.ClassLossTolerant] = unsupported
	mapping.Client[run.ClassLossTolerant] = unsupported
	refreshMappingHashes(&mapping)

	report := Evaluate(cfg, testTransport, mapping, nil)
	if report.Outcome != run.OutcomeUnsupported {
		t.Fatalf("outcome=%s reasons=%v", report.Outcome, report.Reasons)
	}
	for _, id := range []ProbeCaseID{CaseCleanLossTolerant, CaseLossTolerantClientLoss, CaseLossTolerantServerLoss} {
		if item := findCase(t, report, id); item.Outcome != run.OutcomeUnsupported {
			t.Fatalf("case %s=%+v", id, item)
		}
	}
}

func TestEvidenceHashIsOrderIndependent(t *testing.T) {
	cfg := DefaultConfig(1)
	mapping := validMapping()
	evidence := passingEvidence(cfg, mapping)
	firstReport := Evaluate(cfg, testTransport, mapping, evidence)
	for left, right := 0, len(evidence)-1; left < right; left, right = left+1, right-1 {
		evidence[left], evidence[right] = evidence[right], evidence[left]
	}
	secondReport := Evaluate(cfg, testTransport, mapping, evidence)
	if firstReport.EvidenceSHA256 != secondReport.EvidenceSHA256 {
		t.Fatalf("evidence hash changed with input order: %s != %s", firstReport.EvidenceSHA256, secondReport.EvidenceSHA256)
	}
	if run.HashValue(firstReport.Cases) != run.HashValue(secondReport.Cases) {
		t.Fatal("case report changed with evidence input order")
	}
}

func TestEvaluateRejectsReusedRawRunAndAttemptIdentity(t *testing.T) {
	cfg := DefaultConfig(1)
	mapping := validMapping()
	evidence := passingEvidence(cfg, mapping)
	evidence[1].ResultSHA256 = evidence[0].ResultSHA256
	report := Evaluate(cfg, testTransport, mapping, evidence)
	if report.Outcome != run.OutcomeInvalid || !reasonsContain(report.Reasons, "duplicate result_sha256") {
		t.Fatalf("reused result accepted: outcome=%s reasons=%v", report.Outcome, report.Reasons)
	}

	evidence = passingEvidence(cfg, mapping)
	evidence[1].RunIdentity = evidence[0].RunIdentity
	evidence[1].AcquisitionID = AcquisitionIdentity(evidence[1].CaseIdentity, evidence[1].AttemptNumber, evidence[1].RunIdentity)
	report = Evaluate(cfg, testTransport, mapping, evidence)
	if report.Outcome != run.OutcomeInvalid || !reasonsContain(report.Reasons, "duplicate run_identity") {
		t.Fatalf("reused run identity accepted: outcome=%s reasons=%v", report.Outcome, report.Reasons)
	}

	evidence = passingEvidence(cfg, mapping)
	evidence[0].AcquisitionID = hashOf("unbound acquisition")
	report = Evaluate(cfg, testTransport, mapping, evidence)
	if report.Outcome != run.OutcomeInvalid || !reasonsContain(report.Reasons, "planned attempt identity") {
		t.Fatalf("unbound acquisition accepted: outcome=%s reasons=%v", report.Outcome, report.Reasons)
	}
}

func TestProbeIdentityBindsConfigAndMappingButNotEvidence(t *testing.T) {
	cfg := DefaultConfig(1)
	mapping := validMapping()
	evidence := passingEvidence(cfg, mapping)
	base := Evaluate(cfg, testTransport, mapping, evidence)

	for left, right := 0, len(evidence)-1; left < right; left, right = left+1, right-1 {
		evidence[left], evidence[right] = evidence[right], evidence[left]
	}
	reordered := Evaluate(cfg, testTransport, mapping, evidence)
	if reordered.ProbeIdentity != base.ProbeIdentity || reordered.EvidenceSHA256 != base.EvidenceSHA256 {
		t.Fatalf("evidence order changed identities: base=%+v reordered=%+v", base, reordered)
	}

	changedEvidence := append([]EvidenceRun(nil), evidence...)
	changedEvidence[0].ResultSHA256 = hashOf("new result")
	changed := Evaluate(cfg, testTransport, mapping, changedEvidence)
	if changed.ProbeIdentity != base.ProbeIdentity || changed.EvidenceSHA256 == base.EvidenceSHA256 {
		t.Fatalf("evidence content was not separated from probe identity")
	}

	changedConfig := cfg
	changedConfig.FamilyAlpha = 0.02
	if got := Evaluate(changedConfig, testTransport, mapping, evidence).ProbeIdentity; got == base.ProbeIdentity {
		t.Fatal("config change did not change probe identity")
	}

	changedMapping := validMapping()
	lt := changedMapping.Server[run.ClassLossTolerant]
	lt.Primitive = "different-datagram"
	changedMapping.Server[run.ClassLossTolerant] = lt
	changedMapping.Client[run.ClassLossTolerant] = lt
	refreshMappingHashes(&changedMapping)
	if got := Evaluate(cfg, testTransport, changedMapping, evidence).ProbeIdentity; got == base.ProbeIdentity {
		t.Fatal("mapping change did not change probe identity")
	}
}

func TestProbabilityNoTargetLoss(t *testing.T) {
	got := probabilityNoTargetLossUpperBound(1, 1000)
	want := math.Pow(0.99, 1000)
	if math.Abs(got-want) > 1e-15 {
		t.Fatalf("p_zero_upper_bound=%g, want %g", got, want)
	}
}

func TestTargetPacketTrialsUsesConservativeByteLowerBound(t *testing.T) {
	if got := targetPacketTrials(1000, 1000, 1500); got != 667 {
		t.Fatalf("target packet trials=%d, want 667", got)
	}
}

func TestTargetTrafficLowerBoundsRejectOverflow(t *testing.T) {
	if _, _, ok := targetTrafficLowerBounds(math.MaxUint64, 2, 1500); ok {
		t.Fatal("overflowing eligible payload traffic was accepted")
	}
}

func TestTargetPacketTrialsUseEligibleDeliveredRatherThanAcceptedSubmissions(t *testing.T) {
	cfg := DefaultConfig(1)
	mapping := validMapping()
	evidence := passingEvidence(cfg, mapping)
	item := evidenceByID(t, evidence, CaseLossTolerantClientLoss)
	item.DeliveredUnique = 600
	item.EligibleDeliveredUnique = 300

	report := Evaluate(cfg, testTransport, mapping, evidence)
	caseReport := findCase(t, report, CaseLossTolerantClientLoss)
	if caseReport.ApplicationTrials != 1000 || caseReport.EligibleDeliveredUnique != 300 || caseReport.Trials != 200 {
		t.Fatalf("trials accepted/eligible/target=%d/%d/%d, want 1000/300/200",
			caseReport.ApplicationTrials, caseReport.EligibleDeliveredUnique, caseReport.Trials)
	}
}

func TestEvaluateUsesFixedValidAcquisitionCount(t *testing.T) {
	config := DefaultConfig(1)
	config.ValidAcquisitionsPerCase = 2
	config.MaxAttemptsPerCase = 3
	mapping := validMapping()

	t.Run("exact count", func(t *testing.T) {
		evidence := repeatEvidence(passingEvidence(config, mapping), 2)
		report := Evaluate(config, testTransport, mapping, evidence)
		if report.Outcome != run.OutcomePass {
			t.Fatalf("outcome=%s reasons=%v", report.Outcome, report.Reasons)
		}
		for _, item := range report.Cases {
			if len(item.EvidenceRuns) != 2 {
				t.Fatalf("case %s evidence=%d, want 2", item.ID, len(item.EvidenceRuns))
			}
		}
	})

	t.Run("fewer is inconclusive", func(t *testing.T) {
		evidence := passingEvidence(config, mapping)
		evidenceByID(t, evidence, CaseMustDeliverClientLoss).SUTFailureReasons = []string{"client crashed"}
		report := Evaluate(config, testTransport, mapping, evidence)
		if report.Outcome != run.OutcomeInconclusive || !reasonsContain(report.Reasons, "valid acquisitions=1") {
			t.Fatalf("outcome=%s reasons=%v", report.Outcome, report.Reasons)
		}
	})

	t.Run("extra is invalid", func(t *testing.T) {
		evidence := repeatEvidence(passingEvidence(config, mapping), 3)
		report := Evaluate(config, testTransport, mapping, evidence)
		if report.Outcome != run.OutcomeInvalid || !reasonsContain(report.Reasons, "exceeds fixed") {
			t.Fatalf("outcome=%s reasons=%v", report.Outcome, report.Reasons)
		}
	})

	t.Run("attempt over fixed ceiling is invalid", func(t *testing.T) {
		evidence := repeatEvidence(passingEvidence(config, mapping), 2)
		evidenceByID(t, evidence, CaseLossTolerantClientLoss).AttemptNumber = config.MaxAttemptsPerCase + 1
		report := Evaluate(config, testTransport, mapping, evidence)
		if report.Outcome != run.OutcomeInvalid || !reasonsContain(report.Reasons, "max_attempts_per_case") {
			t.Fatalf("outcome=%s reasons=%v", report.Outcome, report.Reasons)
		}
	})

	t.Run("duplicate attempt is invalid", func(t *testing.T) {
		evidence := repeatEvidence(passingEvidence(config, mapping), 2)
		seen := 0
		for index := range evidence {
			if evidence[index].CaseID == CaseLossTolerantClientLoss {
				evidence[index].AttemptNumber = 1
				seen++
			}
		}
		if seen != 2 {
			t.Fatalf("mutated attempts=%d, want 2", seen)
		}
		report := Evaluate(config, testTransport, mapping, evidence)
		if report.Outcome != run.OutcomeInvalid || !reasonsContain(report.Reasons, "duplicate attempt_number") {
			t.Fatalf("outcome=%s reasons=%v", report.Outcome, report.Reasons)
		}
	})
}

func TestEvaluateValidatesEvidenceForUnsupportedMapping(t *testing.T) {
	config := DefaultConfig(1)
	mapping := validMapping()
	unsupported := run.ClassMappingSpec{
		Primitive: "none", Delivery: run.ClassMappingDeliveryReliable,
		Ordering: run.ClassMappingOrderingOrdered, Realization: run.ClassMappingRealizationUnsupported,
	}
	mapping.Server[run.ClassLossTolerant] = unsupported
	mapping.Client[run.ClassLossTolerant] = unsupported
	refreshMappingHashes(&mapping)

	evidence := passingEvidence(config, mapping)
	report := Evaluate(config, testTransport, mapping, evidence)
	if item := findCase(t, report, CaseLossTolerantClientLoss); item.Outcome != run.OutcomeUnsupported {
		t.Fatalf("valid supplied evidence should retain unsupported: %+v", item)
	}

	evidenceByID(t, evidence, CaseLossTolerantClientLoss).ResultSHA256 = "broken"
	report = Evaluate(config, testTransport, mapping, evidence)
	if item := findCase(t, report, CaseLossTolerantClientLoss); item.Outcome != run.OutcomeInvalid {
		t.Fatalf("broken supplied evidence must override unsupported: %+v", item)
	}

	evidence = repeatEvidence(passingEvidence(config, mapping), 2)
	report = Evaluate(config, testTransport, mapping, evidence)
	if item := findCase(t, report, CaseLossTolerantClientLoss); item.Outcome != run.OutcomeInvalid ||
		!reasonsContain(item.Reasons, "exceeds fixed") {
		t.Fatalf("extra unsupported evidence bypassed fixed acquisition count: %+v", item)
	}
}

func TestEvaluateRejectsIneligibleDeliveredCount(t *testing.T) {
	config := DefaultConfig(1)
	mapping := validMapping()
	evidence := passingEvidence(config, mapping)
	item := evidenceByID(t, evidence, CaseLossTolerantClientLoss)
	item.EligibleDeliveredUnique = item.DeliveredUnique + 1

	report := Evaluate(config, testTransport, mapping, evidence)
	caseReport := findCase(t, report, CaseLossTolerantClientLoss)
	if caseReport.Outcome != run.OutcomeInvalid || !reasonsContain(caseReport.Reasons, "eligible_delivered_unique") {
		t.Fatalf("case=%+v", caseReport)
	}
}

func TestEvaluateRejectsNonFiniteInputsWithoutHashing(t *testing.T) {
	mapping := validMapping()
	for name, mutate := range map[string]func(*Config){
		"config NaN":      func(config *Config) { config.FamilyAlpha = math.NaN() },
		"config infinity": func(config *Config) { config.LossPercent = math.Inf(1) },
	} {
		t.Run(name, func(t *testing.T) {
			config := DefaultConfig(1)
			mutate(&config)
			report := Evaluate(config, testTransport, mapping, nil)
			if report.Outcome != run.OutcomeInvalid || report.MappingSHA256 != "" || report.ProbeIdentity != "" || report.EvidenceSHA256 != "" {
				t.Fatalf("invalid config reached hashing: %+v", report)
			}
			if _, err := json.Marshal(report); err != nil {
				t.Fatalf("invalid report is not persistable: %v", err)
			}
			_ = run.HashValue(report)
		})
	}

	for name, mutate := range map[string]func(*LossContractEvidence){
		"client percent NaN": func(loss *LossContractEvidence) { loss.ClientEgressLossPercent = math.NaN() },
		"server percent inf": func(loss *LossContractEvidence) { loss.ServerEgressLossPercent = math.Inf(-1) },
		"burst NaN":          func(loss *LossContractEvidence) { loss.LossBurstLength = math.NaN() },
	} {
		t.Run(name, func(t *testing.T) {
			config := DefaultConfig(1)
			evidence := passingEvidence(config, mapping)
			mutate(&evidenceByID(t, evidence, CaseLossTolerantClientLoss).Loss)
			report := Evaluate(config, testTransport, mapping, evidence)
			if report.Outcome != run.OutcomeInvalid || report.EvidenceSHA256 != "" || !reasonsContain(report.Reasons, "must be finite") {
				t.Fatalf("non-finite evidence reached hashing: %+v", report)
			}
		})
	}
}

func TestSUTFailureStillValidatesStaticLossTreatment(t *testing.T) {
	config := DefaultConfig(1)
	mapping := validMapping()
	evidence := passingEvidence(config, mapping)
	item := evidenceByID(t, evidence, CaseLossTolerantClientLoss)
	item.SUTFailureReasons = []string{"client crashed"}
	item.Loss.ClientEgressLossPercent = 0
	item.Loss.ServerEgressLossPercent = config.LossPercent

	report := Evaluate(config, testTransport, mapping, evidence)
	caseReport := findCase(t, report, CaseLossTolerantClientLoss)
	if caseReport.Outcome != run.OutcomeInvalid || !reasonsContain(caseReport.Reasons, "not client-egress-only") {
		t.Fatalf("static treatment was skipped after SUT failure: %+v", caseReport)
	}
}

func TestEvaluateCopiesMappingAndEvidenceDeeply(t *testing.T) {
	config := DefaultConfig(1)
	mapping := validMapping()
	evidence := passingEvidence(config, mapping)
	item := evidenceByID(t, evidence, CaseMustDeliverClientLoss)
	item.SUTFailureReasons = []string{"original failure"}
	report := Evaluate(config, testTransport, mapping, evidence)
	before := run.HashValue(report)

	delete(mapping.Server, run.ClassMustDeliver)
	mapping.Client[run.ClassLossTolerant] = run.ClassMappingSpec{}
	item.SUTFailureReasons[0] = "mutated failure"
	item.Transport = "other"
	if got := run.HashValue(report); got != before {
		t.Fatalf("report changed after caller mutation: before=%s after=%s", before, got)
	}
	caseReport := findCase(t, report, CaseMustDeliverClientLoss)
	if got := caseReport.EvidenceRuns[0].SUTFailureReasons[0]; got != "original failure" {
		t.Fatalf("nested evidence slice was not copied: %q", got)
	}
}

func TestLossPassRequiresCorrespondingCleanPass(t *testing.T) {
	config := DefaultConfig(1)
	mapping := validMapping()
	for _, testCase := range []struct {
		clean ProbeCaseID
		loss  []ProbeCaseID
	}{
		{CaseCleanLossTolerant, []ProbeCaseID{CaseLossTolerantClientLoss, CaseLossTolerantServerLoss}},
		{CaseCleanMustDeliver, []ProbeCaseID{CaseMustDeliverClientLoss, CaseMustDeliverServerLoss}},
	} {
		evidence := passingEvidence(config, mapping)
		clean := evidenceByID(t, evidence, testCase.clean)
		clean.DeliveredUnique--
		clean.EligibleDeliveredUnique--
		report := Evaluate(config, testTransport, mapping, evidence)
		for _, id := range testCase.loss {
			item := findCase(t, report, id)
			if item.Outcome != run.OutcomeInconclusive || !reasonsContain(item.Reasons, "clean control") {
				t.Fatalf("case %s retained unattributable PASS: %+v", id, item)
			}
		}
	}
}

func TestTransportBindsProbeAndEvidenceIdentity(t *testing.T) {
	config := DefaultConfig(1)
	mapping := validMapping()
	evidence := passingEvidence(config, mapping)
	base := Evaluate(config, testTransport, mapping, evidence)
	if base.Transport != testTransport || base.ProbeIdentity == "" {
		t.Fatalf("transport was not reported: %+v", base)
	}
	if other := ProbeIdentity(config, "other-transport", MappingSHA256(mapping)); other == base.ProbeIdentity {
		t.Fatal("transport did not change probe identity")
	}

	evidenceByID(t, evidence, CaseMustDeliverClientLoss).Transport = "other-transport"
	report := Evaluate(config, testTransport, mapping, evidence)
	if report.Outcome != run.OutcomeInvalid || !reasonsContain(report.Reasons, "want \"test-transport\"") {
		t.Fatalf("evidence transport mismatch accepted: %+v", report)
	}

	report = Evaluate(config, "../unsafe", mapping, nil)
	if report.Outcome != run.OutcomeInvalid || report.MappingSHA256 != "" || report.ProbeIdentity != "" || report.EvidenceSHA256 != "" {
		t.Fatalf("unsafe transport reached hashing: %+v", report)
	}
}

func validMapping() run.ClassMappingRecord {
	lt := run.ClassMappingSpec{
		Primitive: "datagram", Delivery: run.ClassMappingDeliveryBestEffort,
		Ordering: run.ClassMappingOrderingUnordered, Realization: run.ClassMappingRealizationNative,
	}
	md := run.ClassMappingSpec{
		Primitive: "stream", Delivery: run.ClassMappingDeliveryReliable,
		Ordering: run.ClassMappingOrderingOrdered, Realization: run.ClassMappingRealizationNative,
	}
	mapping := run.ClassMappingRecord{
		Server: map[string]run.ClassMappingSpec{run.ClassLossTolerant: lt, run.ClassMustDeliver: md},
		Client: map[string]run.ClassMappingSpec{run.ClassLossTolerant: lt, run.ClassMustDeliver: md},
		Match:  true,
	}
	refreshMappingHashes(&mapping)
	return mapping
}

func refreshMappingHashes(mapping *run.ClassMappingRecord) {
	mapping.ServerSHA256 = run.HashValue(mapping.Server)
	mapping.ClientSHA256 = run.HashValue(mapping.Client)
	mapping.Match = run.HashValue(mapping.Server) == run.HashValue(mapping.Client)
}

func passingEvidence(cfg Config, mapping run.ClassMappingRecord) []EvidenceRun {
	mappingSHA := MappingSHA256(mapping)
	probeIdentity := ProbeIdentity(cfg, testTransport, mappingSHA)
	makeEvidence := func(id ProbeCaseID, slots, delivered uint64, loss LossContractEvidence) EvidenceRun {
		definition, _ := caseDefinition(id)
		caseIdentity := CaseIdentity(probeIdentity, definition)
		runIdentity := hashOf(string(id) + "/run")
		return EvidenceRun{
			CaseID:        id,
			Transport:     testTransport,
			RunIdentity:   runIdentity,
			AcquisitionID: AcquisitionIdentity(caseIdentity, 1, runIdentity),
			AttemptNumber: 1,
			ResultSHA256:  hashOf(string(id) + "/result"),
			MetricsArtifacts: []MetricsArtifactDigest{
				{RelativePath: "metrics/server.json", SizeBytes: 1, SHA256: hashOf(string(id) + "/server-metrics")},
				{RelativePath: "metrics/client-0.json", SizeBytes: 1, SHA256: hashOf(string(id) + "/client-metrics")},
			},
			MappingSHA256:           mappingSHA,
			EndpointMappingSHA256:   endpointMappingSHA256(mapping, definition, mappingSHA),
			CaseIdentity:            caseIdentity,
			MeasurementValid:        true,
			Scenario:                run.ScenarioEnvironmentBaseline,
			TrafficClass:            definition.Class,
			ClassExclusive:          true,
			Echo:                    true,
			PayloadPatternVerified:  true,
			WireCompressionDisabled: true,
			ServerCoalescing:        "none",
			ClientCoalescing:        "none",
			OffloadsDisabled:        true,
			Slots:                   slots,
			Submitted:               slots,
			ExpectedReceives:        slots,
			DeliveredUnique:         delivered,
			EligibleDeliveredUnique: delivered,
			Loss:                    loss,
		}
	}
	clean := LossContractEvidence{Mode: LossModeNone}
	clientLoss := LossContractEvidence{
		Mode: LossModeRandomNetem, Supported: true, Scope: LossEvidenceScopeEffectiveInner,
		ClientEgressLossPercent: cfg.LossPercent, ClientEgressDropped: 10,
		ClientEgressSentBytes: cfg.SlotsPerRun * cfg.PayloadBytes, ClientEgressSentPackets: cfg.SlotsPerRun,
		RandomLossOnly: true,
	}
	serverLoss := LossContractEvidence{
		Mode: LossModeRandomNetem, Supported: true, Scope: LossEvidenceScopeEffectiveInner,
		ServerEgressLossPercent: cfg.LossPercent, ServerEgressDropped: 10,
		ServerEgressSentBytes: cfg.SlotsPerRun * cfg.PayloadBytes, ServerEgressSentPackets: cfg.SlotsPerRun,
		RandomLossOnly: true,
	}
	evidence := []EvidenceRun{
		makeEvidence(CaseCleanLossTolerant, cfg.SlotsPerRun, cfg.SlotsPerRun, clean),
		makeEvidence(CaseCleanMustDeliver, cfg.SlotsPerRun, cfg.SlotsPerRun, clean),
		makeEvidence(CaseLossTolerantClientLoss, cfg.SlotsPerRun, cfg.SlotsPerRun-10, clientLoss),
		makeEvidence(CaseLossTolerantServerLoss, cfg.SlotsPerRun, cfg.SlotsPerRun-10, serverLoss),
		makeEvidence(CaseMustDeliverClientLoss, cfg.SlotsPerRun, cfg.SlotsPerRun, clientLoss),
		makeEvidence(CaseMustDeliverServerLoss, cfg.SlotsPerRun, cfg.SlotsPerRun, serverLoss),
	}
	for index := range evidence {
		if definition, _ := caseDefinition(evidence[index].CaseID); !definition.Clean {
			evidence[index].LossEvidenceSHA256 = hashOf(string(evidence[index].CaseID) + "/loss")
		}
	}
	return evidence
}

func repeatEvidence(base []EvidenceRun, count int) []EvidenceRun {
	result := make([]EvidenceRun, 0, len(base)*count)
	for repetition := 0; repetition < count; repetition++ {
		for _, item := range base {
			copy := item
			suffix := fmt.Sprintf("/%d", repetition)
			copy.RunIdentity = hashOf(string(item.CaseID) + "/run" + suffix)
			copy.AttemptNumber = repetition + 1
			copy.AcquisitionID = AcquisitionIdentity(copy.CaseIdentity, copy.AttemptNumber, copy.RunIdentity)
			copy.ResultSHA256 = hashOf(string(item.CaseID) + "/result" + suffix)
			if copy.LossEvidenceSHA256 != "" {
				copy.LossEvidenceSHA256 = hashOf(string(item.CaseID) + "/loss" + suffix)
			}
			result = append(result, copy)
		}
	}
	return result
}

func evidenceByID(t *testing.T, evidence []EvidenceRun, id ProbeCaseID) *EvidenceRun {
	t.Helper()
	for index := range evidence {
		if evidence[index].CaseID == id {
			return &evidence[index]
		}
	}
	t.Fatalf("case %s not found", id)
	return nil
}

func findCase(t *testing.T, report Report, id ProbeCaseID) CaseReport {
	t.Helper()
	for _, item := range report.Cases {
		if item.ID == id {
			return item
		}
	}
	t.Fatalf("case %s not found", id)
	return CaseReport{}
}

func hashOf(value string) string {
	return run.HashBytes([]byte(value))
}

func reasonsContain(reasons []string, needle string) bool {
	for _, reason := range reasons {
		if strings.Contains(reason, needle) {
			return true
		}
	}
	return false
}

func setSlots(config *Config, slots uint64) {
	config.SlotsPerRun = slots
	config.DurationNS = slots * (nanosecondsPerSecond / config.RateHz)
}
