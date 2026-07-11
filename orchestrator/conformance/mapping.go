package conformance

import (
	"encoding/hex"
	"fmt"
	"math"
	"path/filepath"
	"sort"
	"strings"

	"github.com/neguse/rudp-bench/orchestrator/run"
)

type LossMode string

const (
	LossModeNone        LossMode = "none"
	LossModeRandomNetem LossMode = "random_netem_qdisc"

	LossEvidenceScopeEffectiveInner = "effective_measurement_window_inner"
)

type LossContractEvidence struct {
	Mode                    LossMode `json:"mode"`
	Supported               bool     `json:"supported"`
	Scope                   string   `json:"scope,omitempty"`
	ClientEgressLossPercent float64  `json:"client_egress_loss_percent"`
	ServerEgressLossPercent float64  `json:"server_egress_loss_percent"`
	ClientEgressDropped     uint64   `json:"client_egress_dropped"`
	ServerEgressDropped     uint64   `json:"server_egress_dropped"`
	ClientEgressSentBytes   uint64   `json:"client_egress_sent_bytes"`
	ClientEgressSentPackets uint64   `json:"client_egress_sent_packets"`
	ServerEgressSentBytes   uint64   `json:"server_egress_sent_bytes"`
	ServerEgressSentPackets uint64   `json:"server_egress_sent_packets"`
	LossSeed                uint64   `json:"loss_seed,omitempty"`
	LossBurstLength         float64  `json:"loss_burst_length,omitempty"`
	RandomLossOnly          bool     `json:"random_loss_only"`
	QueueOverflowDrops      uint64   `json:"queue_overflow_drops"`
}

type MetricsArtifactDigest struct {
	RelativePath string `json:"relative_path"`
	SizeBytes    uint64 `json:"size_bytes"`
	SHA256       string `json:"sha256"`
}

// EvidenceRun is an already-extracted, immutable summary of one class-exclusive
// environment_baseline echo run. Extraction and socket execution deliberately
// live outside this package; Evaluate is a pure evidence-to-report function.
type EvidenceRun struct {
	CaseID                  ProbeCaseID             `json:"case_id"`
	Transport               string                  `json:"transport"`
	RunIdentity             string                  `json:"run_identity"`
	AcquisitionID           string                  `json:"acquisition_id"`
	AttemptNumber           int                     `json:"attempt_number"`
	ResultSHA256            string                  `json:"result_sha256"`
	MetricsArtifacts        []MetricsArtifactDigest `json:"metrics_artifacts,omitempty"`
	LossEvidenceSHA256      string                  `json:"loss_evidence_sha256,omitempty"`
	MappingSHA256           string                  `json:"mapping_sha256"`
	EndpointMappingSHA256   string                  `json:"endpoint_mapping_sha256"`
	CaseIdentity            string                  `json:"case_identity"`
	MeasurementValid        bool                    `json:"measurement_valid"`
	InvalidReasons          []string                `json:"invalid_reasons,omitempty"`
	SUTFailureReasons       []string                `json:"sut_failure_reasons,omitempty"`
	Scenario                run.ScenarioKind        `json:"scenario"`
	TrafficClass            string                  `json:"traffic_class"`
	ClassExclusive          bool                    `json:"class_exclusive"`
	Echo                    bool                    `json:"echo"`
	PayloadPatternVerified  bool                    `json:"payload_pattern_verified"`
	WireCompressionDisabled bool                    `json:"wire_compression_disabled"`
	ServerCoalescing        string                  `json:"server_coalescing"`
	ClientCoalescing        string                  `json:"client_coalescing"`
	OffloadsDisabled        bool                    `json:"offloads_disabled"`
	Slots                   uint64                  `json:"slots"`
	Submitted               uint64                  `json:"submitted"`
	ExpectedReceives        uint64                  `json:"expected_receives"`
	DeliveredUnique         uint64                  `json:"delivered_unique"`
	EligibleDeliveredUnique uint64                  `json:"eligible_delivered_unique"`
	Duplicates              uint64                  `json:"duplicates"`
	Corruption              uint64                  `json:"corruption"`
	Loss                    LossContractEvidence    `json:"loss"`
}

type ObservationScope struct {
	Delivery    bool     `json:"delivery"`
	Ordering    bool     `json:"ordering"`
	Primitive   bool     `json:"primitive"`
	Realization bool     `json:"realization"`
	Limitations []string `json:"limitations"`
}

func deliveryObservationScope(config Config) ObservationScope {
	return ObservationScope{
		Delivery: true,
		Limitations: []string{
			"ordering is declared metadata and is not observed by this delivery probe",
			"primitive is declared metadata and is not identified by this delivery probe",
			"realization is declared metadata and is not distinguished by this delivery probe",
			"zero application loss cannot falsify best_effort without a separately verified no-recovery contract",
			fmt.Sprintf("reliable delivery is observed as delivered_by_drain at the fixed drain_ns=%d; later delivery is outside this probe", config.DrainNS),
		},
	}
}

type CaseReport struct {
	CaseDefinition
	DeclaredMapping         *run.ClassMappingSpec `json:"declared_mapping,omitempty"`
	EndpointMappingSHA256   string                `json:"endpoint_mapping_sha256,omitempty"`
	CaseIdentity            string                `json:"case_identity,omitempty"`
	Observed                ObservationScope      `json:"observed"`
	EvidenceRuns            []EvidenceRun         `json:"evidence_runs,omitempty"`
	ApplicationTrials       uint64                `json:"application_trials"`
	Trials                  uint64                `json:"target_packet_trials"`
	ExpectedReceives        uint64                `json:"expected_receives"`
	DeliveredUnique         uint64                `json:"delivered_unique"`
	EligibleDeliveredUnique uint64                `json:"eligible_delivered_unique"`
	Missing                 uint64                `json:"missing"`
	Duplicates              uint64                `json:"duplicates"`
	Corruption              uint64                `json:"corruption"`
	Alpha                   float64               `json:"alpha"`
	PZeroUpperBound         float64               `json:"p_zero_upper_bound"`
	Outcome                 run.Outcome           `json:"outcome"`
	Reasons                 []string              `json:"reasons,omitempty"`
}

type Report struct {
	Version          int                    `json:"version"`
	GateVersion      string                 `json:"gate_version"`
	Transport        string                 `json:"transport"`
	Config           Config                 `json:"config"`
	MappingSHA256    string                 `json:"mapping_sha256,omitempty"`
	ProbeIdentity    string                 `json:"probe_identity,omitempty"`
	EvidenceSHA256   string                 `json:"evidence_sha256,omitempty"`
	Observed         ObservationScope       `json:"observed"`
	EndpointMappings run.ClassMappingRecord `json:"endpoint_mappings"`
	Cases            []CaseReport           `json:"cases"`
	Outcome          run.Outcome            `json:"outcome"`
	Reasons          []string               `json:"reasons,omitempty"`
}

// MappingSHA256 binds the two endpoint declarations without trusting their
// stored hashes or the derived Match flag.
func MappingSHA256(mapping run.ClassMappingRecord) string {
	return run.HashValue(struct {
		Server map[string]run.ClassMappingSpec `json:"server"`
		Client map[string]run.ClassMappingSpec `json:"client"`
	}{mapping.Server, mapping.Client})
}

// ProbeIdentity identifies the declared treatment and gate contract without
// incorporating acquired evidence. Repeating the same probe therefore retains
// this identity while EvidenceSHA256 changes with the observations.
func ProbeIdentity(config Config, transport, mappingSHA256 string) string {
	if len(config.Validate()) > 0 || !run.IsSafeName(transport) {
		return ""
	}
	return run.HashValue(struct {
		GateVersion   string `json:"gate_version"`
		Config        Config `json:"config"`
		Transport     string `json:"transport"`
		MappingSHA256 string `json:"mapping_sha256"`
	}{GateVersion, config, transport, mappingSHA256})
}

func CaseIdentity(probeIdentity string, definition CaseDefinition) string {
	return run.HashValue(struct {
		ProbeIdentity string         `json:"probe_identity"`
		Case          CaseDefinition `json:"case"`
	}{probeIdentity, definition})
}

// AcquisitionIdentity binds a planned attempt to its case and exact run
// treatment before result evidence exists.
func AcquisitionIdentity(caseIdentity string, attemptNumber int, runIdentity string) string {
	if !isSHA256(caseIdentity) || !isSHA256(runIdentity) || attemptNumber <= 0 {
		return ""
	}
	return run.HashValue(struct {
		Version       int    `json:"version"`
		CaseIdentity  string `json:"case_identity"`
		AttemptNumber int    `json:"attempt_number"`
		RunIdentity   string `json:"run_identity"`
	}{1, caseIdentity, attemptNumber, runIdentity})
}

func cloneMapping(mapping run.ClassMappingRecord) run.ClassMappingRecord {
	cloneSpecs := func(values map[string]run.ClassMappingSpec) map[string]run.ClassMappingSpec {
		if values == nil {
			return nil
		}
		cloned := make(map[string]run.ClassMappingSpec, len(values))
		for class, spec := range values {
			cloned[class] = spec
		}
		return cloned
	}
	mapping.Server = cloneSpecs(mapping.Server)
	mapping.Client = cloneSpecs(mapping.Client)
	return mapping
}

func cloneEvidenceRuns(evidence []EvidenceRun) []EvidenceRun {
	if evidence == nil {
		return nil
	}
	cloned := make([]EvidenceRun, len(evidence))
	copy(cloned, evidence)
	for index := range cloned {
		cloned[index].InvalidReasons = append([]string(nil), evidence[index].InvalidReasons...)
		cloned[index].SUTFailureReasons = append([]string(nil), evidence[index].SUTFailureReasons...)
		cloned[index].MetricsArtifacts = append([]MetricsArtifactDigest(nil), evidence[index].MetricsArtifacts...)
	}
	return cloned
}

func validateEvidenceFinite(evidence []EvidenceRun) []string {
	var reasons []string
	for index, item := range evidence {
		for name, value := range map[string]float64{
			"client_egress_loss_percent": item.Loss.ClientEgressLossPercent,
			"server_egress_loss_percent": item.Loss.ServerEgressLossPercent,
			"loss_burst_length":          item.Loss.LossBurstLength,
		} {
			if math.IsNaN(value) || math.IsInf(value, 0) {
				reasons = append(reasons, fmt.Sprintf("evidence[%d] %s=%g must be finite", index, name, value))
			}
		}
	}
	return dedupeSorted(reasons)
}

func reportableConfig(config Config) Config {
	if math.IsNaN(config.FamilyAlpha) || math.IsInf(config.FamilyAlpha, 0) {
		config.FamilyAlpha = 0
	}
	if math.IsNaN(config.LossPercent) || math.IsInf(config.LossPercent, 0) {
		config.LossPercent = 0
	}
	return config
}

// Evaluate validates the fixed six-case evidence matrix and compares observed
// delivery behavior with each sending endpoint's structured declaration.
func Evaluate(config Config, transport string, mapping run.ClassMappingRecord, evidence []EvidenceRun) Report {
	mapping = cloneMapping(mapping)
	evidence = cloneEvidenceRuns(evidence)
	report := Report{
		Version:          ReportVersion,
		GateVersion:      GateVersion,
		Transport:        transport,
		Config:           reportableConfig(config),
		Observed:         deliveryObservationScope(config),
		EndpointMappings: mapping,
		Outcome:          run.OutcomePass,
	}

	if reasons := config.Validate(); len(reasons) > 0 {
		report.Outcome = run.OutcomeInvalid
		report.Reasons = append(report.Reasons, reasons...)
		report.Reasons = dedupeSorted(report.Reasons)
		return report
	}
	if !run.IsSafeName(transport) {
		report.Outcome = run.OutcomeInvalid
		report.Reasons = []string{fmt.Sprintf("transport=%q must be a path-safe ASCII slug", transport)}
		return report
	}
	report.MappingSHA256 = MappingSHA256(mapping)
	report.ProbeIdentity = ProbeIdentity(config, transport, report.MappingSHA256)
	if reasons := validateEvidenceFinite(evidence); len(reasons) > 0 {
		report.Outcome = run.OutcomeInvalid
		report.Reasons = dedupeSorted(append(report.Reasons, reasons...))
		return report
	}
	report.EvidenceSHA256 = evidenceSHA256(evidence)
	if reasons := run.ValidateClassMappingRecord(mapping); len(reasons) > 0 {
		report.Outcome = run.OutcomeInvalid
		for _, reason := range reasons {
			report.Reasons = append(report.Reasons, "class mapping: "+reason)
		}
	}

	grouped := make(map[ProbeCaseID][]EvidenceRun, len(requiredCases))
	seenAcquisition := map[string]bool{}
	seenRunIdentity := map[string]bool{}
	seenResult := map[string]bool{}
	for _, item := range evidence {
		if _, ok := caseDefinition(item.CaseID); !ok {
			report.Outcome = run.OutcomeInvalid
			report.Reasons = append(report.Reasons, fmt.Sprintf("unknown evidence case %q", item.CaseID))
			continue
		}
		if item.AcquisitionID != "" && seenAcquisition[item.AcquisitionID] {
			report.Outcome = run.OutcomeInvalid
			report.Reasons = append(report.Reasons, fmt.Sprintf("duplicate acquisition_id %q", item.AcquisitionID))
		}
		seenAcquisition[item.AcquisitionID] = true
		if item.RunIdentity != "" && seenRunIdentity[item.RunIdentity] {
			report.Outcome = run.OutcomeInvalid
			report.Reasons = append(report.Reasons, fmt.Sprintf("duplicate run_identity %q", item.RunIdentity))
		}
		seenRunIdentity[item.RunIdentity] = true
		if item.ResultSHA256 != "" && seenResult[item.ResultSHA256] {
			report.Outcome = run.OutcomeInvalid
			report.Reasons = append(report.Reasons, fmt.Sprintf("duplicate result_sha256 %q", item.ResultSHA256))
		}
		seenResult[item.ResultSHA256] = true
		grouped[item.CaseID] = append(grouped[item.CaseID], item)
	}

	baseOutcome := report.Outcome
	baseReasons := append([]string(nil), report.Reasons...)
	for _, definition := range requiredCases {
		caseReport := evaluateCase(config, transport, mapping, definition, grouped[definition.ID], report.MappingSHA256)
		report.Cases = append(report.Cases, caseReport)
	}
	applyCleanDependencies(report.Cases)
	report.Outcome = baseOutcome
	report.Reasons = baseReasons
	for _, caseReport := range report.Cases {
		report.Outcome = worseOutcome(report.Outcome, caseReport.Outcome)
		for _, reason := range caseReport.Reasons {
			report.Reasons = append(report.Reasons, fmt.Sprintf("%s: %s", caseReport.ID, reason))
		}
	}
	report.Reasons = dedupeSorted(report.Reasons)
	return report
}

func applyCleanDependencies(cases []CaseReport) {
	type cleanResult struct {
		id      ProbeCaseID
		outcome run.Outcome
	}
	cleanByClass := make(map[string]cleanResult)
	for _, item := range cases {
		if item.Clean {
			cleanByClass[item.Class] = cleanResult{id: item.ID, outcome: item.Outcome}
		}
	}
	for index := range cases {
		item := &cases[index]
		if item.Clean || item.Outcome != run.OutcomePass {
			continue
		}
		clean, ok := cleanByClass[item.Class]
		if !ok || clean.outcome != run.OutcomePass {
			item.Outcome = run.OutcomeInconclusive
			if ok {
				item.Reasons = append(item.Reasons, fmt.Sprintf(
					"corresponding clean control %s outcome=%s; loss-case PASS is not attributable",
					clean.id, clean.outcome))
			} else {
				item.Reasons = append(item.Reasons, "corresponding clean control is missing; loss-case PASS is not attributable")
			}
			item.Reasons = dedupeSorted(item.Reasons)
		}
	}
}

func evaluateCase(config Config, transport string, mapping run.ClassMappingRecord, definition CaseDefinition, evidence []EvidenceRun, mappingSHA string) CaseReport {
	probeIdentity := ProbeIdentity(config, transport, mappingSHA)
	evidence = sortedEvidence(evidence)
	report := CaseReport{
		CaseDefinition:        definition,
		EndpointMappingSHA256: endpointMappingSHA256(mapping, definition, mappingSHA),
		CaseIdentity:          CaseIdentity(probeIdentity, definition),
		Observed:              deliveryObservationScope(config),
		EvidenceRuns:          append([]EvidenceRun(nil), evidence...),
		Outcome:               run.OutcomePass,
	}
	spec, supported, reasons := mappingForCase(mapping, definition)
	if spec != nil {
		copy := *spec
		report.DeclaredMapping = &copy
	}
	if len(reasons) > 0 {
		report.Outcome = run.OutcomeInvalid
		report.Reasons = append(report.Reasons, reasons...)
		return report
	}
	exactSubmission := true
	seenAttempts := make(map[int]bool, len(evidence))
	for index, item := range evidence {
		context := fmt.Sprintf("evidence[%d]", index)
		if seenAttempts[item.AttemptNumber] {
			report.Outcome = run.OutcomeInvalid
			report.Reasons = append(report.Reasons, fmt.Sprintf(
				"%s: duplicate attempt_number=%d", context, item.AttemptNumber))
		}
		seenAttempts[item.AttemptNumber] = true
		reasons := validateEvidenceRun(config, transport, definition, item, mappingSHA, report.EndpointMappingSHA256, report.CaseIdentity)
		if len(reasons) > 0 {
			report.Outcome = run.OutcomeInvalid
			for _, reason := range reasons {
				report.Reasons = append(report.Reasons, context+": "+reason)
			}
			continue
		}
		if len(item.SUTFailureReasons) > 0 {
			if !supported {
				continue
			}
			report.Outcome = worseOutcome(report.Outcome, run.OutcomeFail)
			for _, reason := range item.SUTFailureReasons {
				report.Reasons = append(report.Reasons, context+": SUT failure: "+reason)
			}
			continue
		}
		if !supported {
			continue
		}
		if item.Submitted != item.Slots {
			exactSubmission = false
		}
		if err := accumulateEvidence(&report, item, config); err != nil {
			report.Outcome = run.OutcomeInvalid
			report.Reasons = append(report.Reasons, context+": "+err.Error())
		}
	}
	if report.Outcome == run.OutcomeInvalid {
		return report
	}
	if len(evidence) > config.ValidAcquisitionsPerCase {
		report.Outcome = run.OutcomeInvalid
		report.Reasons = append(report.Reasons, fmt.Sprintf(
			"valid acquisitions=%d exceeds fixed valid_acquisitions_per_case=%d",
			len(evidence), config.ValidAcquisitionsPerCase))
		return report
	}
	if !supported {
		report.Outcome = run.OutcomeUnsupported
		report.Reasons = append(report.Reasons, "declared mapping is unsupported")
		return report
	}
	if len(evidence) < config.ValidAcquisitionsPerCase {
		report.Outcome = run.OutcomeInconclusive
		if len(evidence) == 0 {
			report.Reasons = append(report.Reasons, fmt.Sprintf(
				"required evidence was not acquired: valid acquisitions=0, want fixed valid_acquisitions_per_case=%d",
				config.ValidAcquisitionsPerCase))
		} else {
			report.Reasons = append(report.Reasons, fmt.Sprintf(
				"valid acquisitions=%d, want fixed valid_acquisitions_per_case=%d",
				len(evidence), config.ValidAcquisitionsPerCase))
		}
		return report
	}
	if report.Outcome == run.OutcomeFail {
		return report
	}
	if report.DeliveredUnique > report.ExpectedReceives {
		report.Outcome = run.OutcomeInvalid
		report.Reasons = append(report.Reasons, "aggregate delivered_unique exceeds expected_receives")
		return report
	}
	report.Missing = report.ExpectedReceives - report.DeliveredUnique

	if report.Corruption > 0 {
		report.Outcome = run.OutcomeFail
		report.Reasons = append(report.Reasons, fmt.Sprintf("payload corruption=%d", report.Corruption))
		return report
	}
	if report.Duplicates > 0 && (definition.Clean || spec.Delivery == run.ClassMappingDeliveryReliable) {
		report.Outcome = run.OutcomeFail
		if definition.Clean {
			report.Reasons = append(report.Reasons, fmt.Sprintf("clean control duplicates=%d", report.Duplicates))
		} else {
			report.Reasons = append(report.Reasons, fmt.Sprintf("declared reliable but duplicates=%d", report.Duplicates))
		}
		return report
	}
	if !exactSubmission {
		report.Outcome = run.OutcomeInconclusive
		report.Reasons = append(report.Reasons, "submitted slots are incomplete; delivery behavior cannot be attributed")
		return report
	}
	if definition.Clean {
		return evaluateCleanCase(report)
	}
	return evaluateLossCase(config, report, *spec)
}

func accumulateEvidence(report *CaseReport, item EvidenceRun, config Config) error {
	packetTrials := targetPacketTrials(item.EligibleDeliveredUnique, config.PayloadBytes, config.LinkMTUBytes)
	values := []struct {
		name  string
		dst   *uint64
		value uint64
	}{
		{"application_trials", &report.ApplicationTrials, item.Submitted},
		{"target_packet_trials", &report.Trials, packetTrials},
		{"expected_receives", &report.ExpectedReceives, item.ExpectedReceives},
		{"delivered_unique", &report.DeliveredUnique, item.DeliveredUnique},
		{"eligible_delivered_unique", &report.EligibleDeliveredUnique, item.EligibleDeliveredUnique},
		{"duplicates", &report.Duplicates, item.Duplicates},
		{"corruption", &report.Corruption, item.Corruption},
	}
	for _, value := range values {
		if ^uint64(0)-*value.dst < value.value {
			return fmt.Errorf("aggregate %s overflows uint64", value.name)
		}
	}
	for _, value := range values {
		*value.dst += value.value
	}
	return nil
}

func evaluateCleanCase(report CaseReport) CaseReport {
	if report.Missing > 0 {
		report.Outcome = run.OutcomeFail
		report.Reasons = append(report.Reasons, fmt.Sprintf("clean control missing=%d", report.Missing))
	}
	if report.ApplicationTrials == 0 && report.Outcome != run.OutcomeFail {
		report.Outcome = run.OutcomeInconclusive
		report.Reasons = append(report.Reasons, "clean control has no application trials")
	}
	return report
}

func evaluateLossCase(config Config, report CaseReport, spec run.ClassMappingSpec) CaseReport {
	report.Alpha = config.CaseAlpha()
	report.PZeroUpperBound = probabilityNoTargetLossUpperBound(config.LossPercent, report.Trials)

	if spec.Delivery == run.ClassMappingDeliveryReliable {
		if report.Missing > 0 {
			report.Outcome = run.OutcomeFail
			report.Reasons = append(report.Reasons, fmt.Sprintf(
				"declared reliable delivered_by_drain=%d below expected_receives=%d at fixed drain_ns=%d (missing=%d)",
				report.DeliveredUnique, report.ExpectedReceives, config.DrainNS, report.Missing))
		}
		if report.Duplicates > 0 {
			report.Outcome = run.OutcomeFail
			report.Reasons = append(report.Reasons, fmt.Sprintf("declared reliable but duplicates=%d", report.Duplicates))
		}
		if report.Outcome == run.OutcomeFail {
			return report
		}
		if !lossExposureSufficient(config.LossPercent, report.Trials, report.Alpha) {
			report.Outcome = run.OutcomeInconclusive
			report.Reasons = append(report.Reasons, fmt.Sprintf("loss exposure insufficient: p_zero_upper_bound=%g exceeds alpha=%g", report.PZeroUpperBound, report.Alpha))
		}
		return report
	}

	if spec.Delivery != run.ClassMappingDeliveryBestEffort {
		report.Outcome = run.OutcomeInvalid
		report.Reasons = append(report.Reasons, fmt.Sprintf("unknown declared delivery behavior %q", spec.Delivery))
		return report
	}
	if report.Missing > 0 {
		return report
	}
	report.Outcome = run.OutcomeInconclusive
	if !lossExposureSufficient(config.LossPercent, report.Trials, report.Alpha) {
		report.Reasons = append(report.Reasons, fmt.Sprintf("loss exposure insufficient: p_zero_upper_bound=%g exceeds alpha=%g", report.PZeroUpperBound, report.Alpha))
		return report
	}
	report.Reasons = append(report.Reasons,
		"no application loss was observed; best_effort permits recovery and cannot be falsified without a no-recovery contract")
	return report
}

func mappingForCase(mapping run.ClassMappingRecord, definition CaseDefinition) (*run.ClassMappingSpec, bool, []string) {
	lookup := func(endpoint string, values map[string]run.ClassMappingSpec) (*run.ClassMappingSpec, bool, []string) {
		spec, ok := values[definition.Class]
		if !ok {
			return nil, false, []string{fmt.Sprintf("%s mapping is missing class %q", endpoint, definition.Class)}
		}
		return &spec, spec.Realization != run.ClassMappingRealizationUnsupported, nil
	}
	switch definition.Endpoint {
	case EndpointClient:
		return lookup("client", mapping.Client)
	case EndpointServer:
		return lookup("server", mapping.Server)
	case EndpointBoth:
		server, serverSupported, serverReasons := lookup("server", mapping.Server)
		client, clientSupported, clientReasons := lookup("client", mapping.Client)
		reasons := append(serverReasons, clientReasons...)
		if len(reasons) > 0 {
			return nil, false, reasons
		}
		if *server != *client {
			return nil, false, []string{"clean control endpoint mappings differ"}
		}
		return server, serverSupported && clientSupported, nil
	default:
		return nil, false, []string{fmt.Sprintf("unknown endpoint %q", definition.Endpoint)}
	}
}

func endpointMappingSHA256(mapping run.ClassMappingRecord, definition CaseDefinition, combined string) string {
	switch definition.Endpoint {
	case EndpointClient:
		return run.HashValue(mapping.Client)
	case EndpointServer:
		return run.HashValue(mapping.Server)
	case EndpointBoth:
		return combined
	default:
		return ""
	}
}

func validateEvidenceRun(config Config, transport string, definition CaseDefinition, item EvidenceRun, mappingSHA, endpointMappingSHA, caseIdentity string) []string {
	var reasons []string
	if !item.MeasurementValid {
		if len(item.InvalidReasons) == 0 {
			reasons = append(reasons, "measurement is invalid without reasons")
		} else {
			for _, reason := range item.InvalidReasons {
				reasons = append(reasons, "measurement invalid: "+reason)
			}
		}
	} else if len(item.InvalidReasons) > 0 {
		reasons = append(reasons, "measurement is valid but has invalid_reasons")
	}
	for name, value := range map[string]string{
		"run_identity":            item.RunIdentity,
		"acquisition_id":          item.AcquisitionID,
		"result_sha256":           item.ResultSHA256,
		"mapping_sha256":          item.MappingSHA256,
		"endpoint_mapping_sha256": item.EndpointMappingSHA256,
		"case_identity":           item.CaseIdentity,
	} {
		if !isSHA256(value) {
			reasons = append(reasons, fmt.Sprintf("%s is not a SHA-256 hex digest", name))
		}
	}
	if item.MappingSHA256 != mappingSHA {
		reasons = append(reasons, fmt.Sprintf("mapping_sha256=%q does not match report mapping %q", item.MappingSHA256, mappingSHA))
	}
	if item.EndpointMappingSHA256 != endpointMappingSHA {
		reasons = append(reasons, fmt.Sprintf("endpoint_mapping_sha256=%q does not match case endpoint mapping %q", item.EndpointMappingSHA256, endpointMappingSHA))
	}
	if item.CaseIdentity != caseIdentity {
		reasons = append(reasons, fmt.Sprintf("case_identity=%q does not match case contract %q", item.CaseIdentity, caseIdentity))
	}
	if item.Transport != transport {
		reasons = append(reasons, fmt.Sprintf("transport=%q, want %q", item.Transport, transport))
	}
	if item.AttemptNumber <= 0 || item.AttemptNumber > config.MaxAttemptsPerCase {
		reasons = append(reasons, fmt.Sprintf(
			"attempt_number=%d must be between 1 and max_attempts_per_case=%d",
			item.AttemptNumber, config.MaxAttemptsPerCase))
	}
	if expected := AcquisitionIdentity(caseIdentity, item.AttemptNumber, item.RunIdentity); item.AcquisitionID != expected {
		reasons = append(reasons, fmt.Sprintf(
			"acquisition_id=%q does not match planned attempt identity %q", item.AcquisitionID, expected))
	}
	if item.Scenario != run.ScenarioEnvironmentBaseline {
		reasons = append(reasons, fmt.Sprintf("scenario=%q, want %q", item.Scenario, run.ScenarioEnvironmentBaseline))
	}
	if item.TrafficClass != definition.Class {
		reasons = append(reasons, fmt.Sprintf("traffic_class=%q, want %q", item.TrafficClass, definition.Class))
	}
	if !item.ClassExclusive {
		reasons = append(reasons, "probe traffic is not class-exclusive")
	}
	if !item.Echo {
		reasons = append(reasons, "probe distribution is not echo")
	}
	if !item.PayloadPatternVerified {
		reasons = append(reasons, "probe payload pattern is not verified")
	}
	if !item.WireCompressionDisabled {
		reasons = append(reasons, "wire compression is not disabled")
	}
	if item.ServerCoalescing == "" || item.ClientCoalescing == "" {
		reasons = append(reasons, "endpoint coalescing disclosure is missing")
	}
	if !item.OffloadsDisabled {
		reasons = append(reasons, "network offloads are not disabled")
	}
	reasons = append(reasons, validateMetricsArtifactDigests(item)...)
	reasons = append(reasons, validateStaticLossContract(config, definition, item.Loss)...)
	if len(item.SUTFailureReasons) > 0 {
		return dedupeSorted(reasons)
	}
	if item.Slots != config.SlotsPerRun {
		reasons = append(reasons, fmt.Sprintf("slots=%d, want configured slots_per_run=%d", item.Slots, config.SlotsPerRun))
	}
	if item.Submitted > item.Slots {
		reasons = append(reasons, fmt.Sprintf("submitted=%d exceeds slots=%d", item.Submitted, item.Slots))
	}
	if item.DeliveredUnique > item.ExpectedReceives {
		reasons = append(reasons, fmt.Sprintf("delivered_unique=%d exceeds expected_receives=%d", item.DeliveredUnique, item.ExpectedReceives))
	}
	if item.DeliveredUnique > item.Submitted {
		reasons = append(reasons, fmt.Sprintf("delivered_unique=%d exceeds submitted=%d for class-exclusive echo", item.DeliveredUnique, item.Submitted))
	}
	if item.EligibleDeliveredUnique > item.DeliveredUnique {
		reasons = append(reasons, fmt.Sprintf(
			"eligible_delivered_unique=%d exceeds delivered_unique=%d",
			item.EligibleDeliveredUnique, item.DeliveredUnique))
	}
	if item.ExpectedReceives != item.Slots {
		reasons = append(reasons, fmt.Sprintf("expected_receives=%d differs from class-exclusive echo slots=%d", item.ExpectedReceives, item.Slots))
	}
	if config.PayloadBytes != 0 && item.Submitted > ^uint64(0)/config.PayloadBytes {
		reasons = append(reasons, "submitted payload byte count overflows uint64")
	}
	if !definition.Clean {
		if !isSHA256(item.LossEvidenceSHA256) {
			reasons = append(reasons, "loss_evidence_sha256 is not a SHA-256 hex digest")
		}
	} else if item.LossEvidenceSHA256 != "" && !isSHA256(item.LossEvidenceSHA256) {
		reasons = append(reasons, "loss_evidence_sha256 is not a SHA-256 hex digest")
	}
	reasons = append(reasons, validateDynamicLossEvidence(definition, item.Loss)...)
	reasons = append(reasons, validateQdiscTrafficCoverage(config, definition, item.EligibleDeliveredUnique, item.Loss)...)
	return dedupeSorted(reasons)
}

func validateMetricsArtifactDigests(item EvidenceRun) []string {
	if len(item.SUTFailureReasons) != 0 && len(item.MetricsArtifacts) == 0 {
		return nil
	}
	var reasons []string
	if len(item.MetricsArtifacts) != 2 {
		reasons = append(reasons, fmt.Sprintf("metrics_artifacts=%d, want server and client snapshots", len(item.MetricsArtifacts)))
	}
	seen := map[string]bool{}
	for index, artifact := range item.MetricsArtifacts {
		clean := filepath.ToSlash(filepath.Clean(filepath.FromSlash(artifact.RelativePath)))
		if artifact.RelativePath == "" || clean != artifact.RelativePath ||
			!strings.HasPrefix(artifact.RelativePath, "metrics/") || !pathWithinRoot(filepath.FromSlash(artifact.RelativePath)) {
			reasons = append(reasons, fmt.Sprintf("metrics_artifacts[%d].relative_path=%q is not a canonical metrics path", index, artifact.RelativePath))
		}
		if seen[artifact.RelativePath] {
			reasons = append(reasons, fmt.Sprintf("metrics_artifacts[%d].relative_path=%q is duplicated", index, artifact.RelativePath))
		}
		seen[artifact.RelativePath] = true
		if artifact.SizeBytes == 0 {
			reasons = append(reasons, fmt.Sprintf("metrics_artifacts[%d].size_bytes must be positive", index))
		}
		if !isSHA256(artifact.SHA256) {
			reasons = append(reasons, fmt.Sprintf("metrics_artifacts[%d].sha256 is not a SHA-256 hex digest", index))
		}
	}
	return reasons
}

func validateStaticLossContract(config Config, definition CaseDefinition, loss LossContractEvidence) []string {
	var reasons []string
	if definition.Clean {
		if loss.Mode != LossModeNone ||
			loss.ClientEgressLossPercent != 0 || loss.ServerEgressLossPercent != 0 ||
			loss.ClientEgressSentBytes != 0 || loss.ClientEgressSentPackets != 0 ||
			loss.ServerEgressSentBytes != 0 || loss.ServerEgressSentPackets != 0 ||
			loss.LossSeed != 0 || loss.LossBurstLength != 0 || loss.RandomLossOnly {
			return []string{"clean control contains a configured loss treatment"}
		}
		return nil
	}
	if loss.Mode != LossModeRandomNetem {
		reasons = append(reasons, fmt.Sprintf("loss mode=%q, want %q", loss.Mode, LossModeRandomNetem))
	}
	if loss.LossSeed != 0 {
		reasons = append(reasons, "loss_seed must be zero for random loss evidence")
	}
	if loss.LossBurstLength != 0 {
		reasons = append(reasons, "loss_burst_length must be zero for independent random loss evidence")
	}
	if !loss.RandomLossOnly {
		reasons = append(reasons, "loss treatment contains or may contain non-random impairments")
	}
	switch definition.Egress {
	case EgressClient:
		if !sameFloat(loss.ClientEgressLossPercent, config.LossPercent) || loss.ServerEgressLossPercent != 0 {
			reasons = append(reasons, fmt.Sprintf("loss treatment is not client-egress-only at %g%%", config.LossPercent))
		}
	case EgressServer:
		if !sameFloat(loss.ServerEgressLossPercent, config.LossPercent) || loss.ClientEgressLossPercent != 0 {
			reasons = append(reasons, fmt.Sprintf("loss treatment is not server-egress-only at %g%%", config.LossPercent))
		}
	default:
		reasons = append(reasons, fmt.Sprintf("loss case has invalid egress %q", definition.Egress))
	}
	return reasons
}

func validateDynamicLossEvidence(definition CaseDefinition, loss LossContractEvidence) []string {
	var reasons []string
	if definition.Clean {
		if loss.Supported || loss.Scope != "" || loss.ClientEgressDropped != 0 || loss.ServerEgressDropped != 0 || loss.QueueOverflowDrops != 0 {
			return []string{"clean control contains loss evidence or loss counters"}
		}
		return nil
	}
	if !loss.Supported {
		reasons = append(reasons, "random netem qdisc evidence is not supported")
	}
	if loss.Scope != LossEvidenceScopeEffectiveInner {
		reasons = append(reasons, fmt.Sprintf("loss evidence scope=%q, want %q", loss.Scope, LossEvidenceScopeEffectiveInner))
	}
	if loss.QueueOverflowDrops != 0 {
		reasons = append(reasons, fmt.Sprintf("queue_overflow_drops=%d, want 0", loss.QueueOverflowDrops))
	}
	switch definition.Egress {
	case EgressClient:
		if loss.ClientEgressDropped == 0 {
			reasons = append(reasons, "client-egress loss exposure has zero observed qdisc drops")
		}
		if loss.ServerEgressDropped != 0 {
			reasons = append(reasons, "unconfigured server egress has observed qdisc drops")
		}
	case EgressServer:
		if loss.ServerEgressDropped == 0 {
			reasons = append(reasons, "server-egress loss exposure has zero observed qdisc drops")
		}
		if loss.ClientEgressDropped != 0 {
			reasons = append(reasons, "unconfigured client egress has observed qdisc drops")
		}
	}
	return reasons
}

func validateQdiscTrafficCoverage(config Config, definition CaseDefinition, eligibleDeliveredUnique uint64, loss LossContractEvidence) []string {
	if definition.Clean {
		return nil
	}
	requiredBytes, requiredPackets, ok := targetTrafficLowerBounds(eligibleDeliveredUnique, config.PayloadBytes, config.LinkMTUBytes)
	if !ok {
		return []string{"eligible target traffic lower bound overflows uint64"}
	}
	var sentBytes, sentPackets uint64
	switch definition.Egress {
	case EgressClient:
		sentBytes, sentPackets = loss.ClientEgressSentBytes, loss.ClientEgressSentPackets
	case EgressServer:
		sentBytes, sentPackets = loss.ServerEgressSentBytes, loss.ServerEgressSentPackets
	default:
		return []string{fmt.Sprintf("loss case has invalid egress %q", definition.Egress)}
	}
	var reasons []string
	if sentBytes < requiredBytes {
		reasons = append(reasons, fmt.Sprintf(
			"configured egress qdisc sent_bytes=%d below eligible payload lower bound=%d",
			sentBytes, requiredBytes))
	}
	if sentPackets < requiredPackets {
		reasons = append(reasons, fmt.Sprintf(
			"configured egress qdisc sent_packets=%d below target packet lower bound=%d",
			sentPackets, requiredPackets))
	}
	return reasons
}

func validateCleanLossContract(loss LossContractEvidence) []string {
	definition := CaseDefinition{Clean: true}
	reasons := validateStaticLossContract(Config{}, definition, loss)
	reasons = append(reasons, validateDynamicLossEvidence(definition, loss)...)
	return dedupeSorted(reasons)
}

func validateOneWayRandomLoss(config Config, egress Egress, loss LossContractEvidence) []string {
	definition := CaseDefinition{Egress: egress}
	reasons := validateStaticLossContract(config, definition, loss)
	reasons = append(reasons, validateDynamicLossEvidence(definition, loss)...)
	return dedupeSorted(reasons)
}

func probabilityNoTargetLossUpperBound(lossPercent float64, trials uint64) float64 {
	if trials == 0 {
		return 1
	}
	logP := float64(trials) * math.Log1p(-lossPercent/100)
	return math.Exp(logP)
}

func lossExposureSufficient(lossPercent float64, trials uint64, alpha float64) bool {
	if trials == 0 || alpha <= 0 || alpha >= 1 || lossPercent <= 0 || lossPercent >= 100 {
		return false
	}
	logP := float64(trials) * math.Log1p(-lossPercent/100)
	return logP <= math.Log(alpha)
}

// targetPacketTrials is a conservative lower bound on independently impaired
// qdisc packets. Eligible deliveries are restricted to packets whose configured
// egress traversal falls inside the qdisc counter's effective inner window.
// Wire compression and offloads are disabled and the payload uses the verified
// splitmix64-v1 pattern, so division by link MTU remains conservative after
// framing. Application pre-submit coalescing is accounted by Submitted; a case
// cannot pass unless every scheduled slot was submitted.
func targetPacketTrials(eligibleDeliveredUnique, payloadBytes, linkMTUBytes uint64) uint64 {
	_, trials, ok := targetTrafficLowerBounds(eligibleDeliveredUnique, payloadBytes, linkMTUBytes)
	if !ok {
		return math.MaxUint64
	}
	return trials
}

func targetTrafficLowerBounds(eligibleDeliveredUnique, payloadBytes, linkMTUBytes uint64) (uint64, uint64, bool) {
	if eligibleDeliveredUnique == 0 || payloadBytes == 0 || linkMTUBytes == 0 {
		return 0, 0, true
	}
	if eligibleDeliveredUnique > math.MaxUint64/payloadBytes {
		return 0, 0, false
	}
	bytes := eligibleDeliveredUnique * payloadBytes
	trials := bytes / linkMTUBytes
	if bytes%linkMTUBytes != 0 {
		trials++
	}
	return bytes, trials, true
}

func evidenceSHA256(evidence []EvidenceRun) string {
	return run.HashValue(sortedEvidence(evidence))
}

func sortedEvidence(evidence []EvidenceRun) []EvidenceRun {
	canonical := append([]EvidenceRun(nil), evidence...)
	sort.Slice(canonical, func(i, j int) bool {
		if canonical[i].CaseID != canonical[j].CaseID {
			return canonical[i].CaseID < canonical[j].CaseID
		}
		if canonical[i].AttemptNumber != canonical[j].AttemptNumber {
			return canonical[i].AttemptNumber < canonical[j].AttemptNumber
		}
		if canonical[i].AcquisitionID != canonical[j].AcquisitionID {
			return canonical[i].AcquisitionID < canonical[j].AcquisitionID
		}
		if canonical[i].RunIdentity != canonical[j].RunIdentity {
			return canonical[i].RunIdentity < canonical[j].RunIdentity
		}
		return run.HashValue(canonical[i]) < run.HashValue(canonical[j])
	})
	return canonical
}

func worseOutcome(a, b run.Outcome) run.Outcome {
	rank := func(outcome run.Outcome) int {
		switch outcome {
		case run.OutcomeInvalid:
			return 5
		case run.OutcomeFail:
			return 4
		case run.OutcomeUnsupported:
			return 3
		case run.OutcomeInconclusive:
			return 2
		case run.OutcomePass:
			return 1
		default:
			return 6
		}
	}
	if rank(b) > rank(a) {
		return b
	}
	return a
}

func isSHA256(value string) bool {
	if len(value) != 64 {
		return false
	}
	_, err := hex.DecodeString(value)
	return err == nil
}

func sameFloat(a, b float64) bool {
	return a == b
}

func dedupeSorted(values []string) []string {
	seen := map[string]bool{}
	for _, value := range values {
		value = strings.TrimSpace(value)
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
