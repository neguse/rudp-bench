package conformance

import (
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"math"
	"os"
	"path/filepath"
	"reflect"
	"strings"

	"github.com/neguse/rudp-bench/orchestrator/control"
	"github.com/neguse/rudp-bench/orchestrator/netops"
	"github.com/neguse/rudp-bench/orchestrator/run"
	"golang.org/x/sys/unix"
)

const (
	resultVersion       = 2
	maxResultBytes      = 64 << 20
	maxMetricsFileBytes = 128 << 20
)

// ExtractRequest binds a raw run artifact to the plan which acquired it. The
// caller must supply identities from the append-only acquisition ledger; none
// of them are inferred from a directory name.
type ExtractRequest struct {
	RunDir                string
	CaseID                ProbeCaseID
	AttemptNumber         int
	AcquisitionID         string
	Config                Config
	Mapping               run.ClassMappingRecord
	ExpectedTransport     string
	ExpectedRunConfig     *run.RunConfig
	ExpectedRunIdentity   string
	ExpectedResultSHA256  string
	ExpectedProbeIdentity string
	ExpectedCaseIdentity  string
}

// Extraction contains hashes useful to an attempt ledger even when the
// artifact is invalid. Evidence is non-nil only after every derivation and
// cross-check has succeeded.
type Extraction struct {
	ResultSHA256   string       `json:"result_sha256"`
	ConfigSHA256   string       `json:"config_sha256,omitempty"`
	InvalidReasons []string     `json:"invalid_reasons,omitempty"`
	Evidence       *EvidenceRun `json:"evidence,omitempty"`
}

// ExtractEvidence verifies and reduces one raw run. Filesystem/read failures
// are returned as errors. Malformed or contradictory benchmark evidence is a
// completed, invalid extraction so the driver can retain it in its attempt
// ledger without accidentally accepting it as an acquisition.
func ExtractEvidence(request ExtractRequest) (Extraction, error) {
	var extraction Extraction
	root, err := secureRunRoot(request.RunDir)
	if err != nil {
		return extraction, err
	}
	rootFD, err := unix.Open(root, unix.O_PATH|unix.O_DIRECTORY|unix.O_CLOEXEC, 0)
	if err != nil {
		return extraction, fmt.Errorf("open run directory: %w", err)
	}
	defer unix.Close(rootFD)
	raw, err := readRegularBeneath(rootFD, "result.json", maxResultBytes)
	if err != nil {
		return extraction, fmt.Errorf("read result.json: %w", err)
	}
	extraction.ResultSHA256 = run.HashBytes(raw)

	var result run.Result
	if err := strictDecode(raw, &result); err != nil {
		extraction.InvalidReasons = []string{"result.json: " + err.Error()}
		return extraction, nil
	}

	definition, caseOK := caseDefinition(request.CaseID)
	if !caseOK {
		extraction.InvalidReasons = []string{fmt.Sprintf("unknown case_id %q", request.CaseID)}
		return extraction, nil
	}
	reasons := validateExtractionBindings(request, result, root, extraction.ResultSHA256, definition)
	extraction.ConfigSHA256 = run.HashValue(result.Config)
	if len(reasons) != 0 {
		extraction.InvalidReasons = dedupeSorted(reasons)
		return extraction, nil
	}

	treatmentEvidence, treatmentReasons := validateExtractionTreatment(result, request.Mapping)
	reasons = append(reasons, treatmentReasons...)
	classSpec, trafficID, scenarioReasons := validateProbeScenario(result.Config, request.Config, definition)
	reasons = append(reasons, scenarioReasons...)

	processes, processIdentityReasons, processCompletenessReasons := validateProbeProcesses(result, request.ExpectedTransport)
	reasons = append(reasons, processIdentityReasons...)
	executionFailures := processFailureReasons(result.Processes)
	executionFailures = append(executionFailures, lifecycleSUTFailureReasons(result)...)
	executionFailures = dedupeSorted(executionFailures)
	networkFailures, networkEvidenceReasons := networkSUTFailureReasons(result)
	reasons = append(reasons, networkEvidenceReasons...)
	sutFailures := dedupeSorted(append(executionFailures, networkFailures...))

	offloadsDisabled := false
	if result.Config.Netem == nil || !result.Config.Netem.DisableOffloads {
		reasons = append(reasons, "netem.disable_offloads is required for packet-exposure evidence")
	} else {
		var before, after *netops.OffloadEvidence
		if result.Netem != nil {
			before, after = result.Netem.Offloads, result.Netem.OffloadsAfter
		}
		if invalid := run.ValidateOffloadIntervalEvidence(&result.Config, before, after, len(sutFailures) != 0); len(invalid) != 0 {
			reasons = append(reasons, invalid...)
		} else {
			offloadsDisabled = true
		}
	}
	if result.Config.Netem != nil && result.Netem != nil {
		if expected := pairSpecForRegime(result.Config.Netem); result.Netem.Pair != expected {
			reasons = append(reasons, "netem pair evidence does not match the configured namespace treatment")
		} else if gateReasons := netops.ValidateNetemGateReport(expected, result.Netem.Gate); len(gateReasons) != 0 {
			reasons = appendPrefixed(reasons, "netem gate evidence: ", gateReasons)
		}
	}

	staticLoss, staticLossReasons := extractStaticLossTreatment(result, request.Config, definition)
	reasons = append(reasons, staticLossReasons...)
	if len(executionFailures) == 0 && len(networkFailures) != 0 {
		reasons = append(reasons, processCompletenessReasons...)
		_, participantReasons := validateProbeParticipants(result)
		reasons = append(reasons, participantReasons...)
	}
	if len(sutFailures) != 0 {
		if len(executionFailures) != 0 && result.Verdict != run.VerdictInvalid {
			reasons = append(reasons, "process failure is paired with a valid run gate")
		}
		if independent := independentResultInvalidReasons(result.InvalidReasons); len(independent) != 0 {
			reasons = appendPrefixed(reasons, "independent run invalidity: ", independent)
		}
		if len(reasons) != 0 {
			extraction.InvalidReasons = dedupeSorted(reasons)
			return extraction, nil
		}
		evidence := baseEvidence(request, definition, extraction.ResultSHA256,
			treatmentEvidence, offloadsDisabled, staticLoss, sutFailures)
		extraction.Evidence = &evidence
		return extraction, nil
	}

	reasons = append(reasons, processCompletenessReasons...)
	corruption, participantReasons := validateProbeParticipants(result)
	reasons = append(reasons, participantReasons...)
	var merged *run.MergedMetrics
	var metricsArtifacts []MetricsArtifactDigest
	if classSpec != nil && len(processIdentityReasons) == 0 && len(processCompletenessReasons) == 0 {
		merged, metricsArtifacts, err = extractMergedMetrics(root, rootFD, result, processes)
		if err != nil {
			reasons = append(reasons, "metrics evidence: "+err.Error())
		}
	}
	var metric run.TrafficAggregate
	if merged != nil && classSpec != nil {
		metric, reasons = extractExclusiveMetric(merged, metric, reasons, trafficID, definition.Class)
		if metric.Submitted != metric.Slots {
			reasons = append(reasons, fmt.Sprintf("selected traffic submitted=%d, want exact slots=%d", metric.Submitted, metric.Slots))
		}
	}
	loss, lossHash, lossReasons := extractLossEvidence(result, request.Config, definition, metric)
	reasons = append(reasons, lossReasons...)
	if len(reasons) != 0 {
		extraction.InvalidReasons = dedupeSorted(reasons)
		return extraction, nil
	}
	if result.Verdict != run.VerdictValid || len(result.InvalidReasons) != 0 {
		extraction.InvalidReasons = []string{"result gate is invalid without a verified SUT failure"}
		return extraction, nil
	}
	evidence := baseEvidence(request, definition, extraction.ResultSHA256,
		treatmentEvidence, offloadsDisabled, loss, nil)
	evidence.Slots = metric.Slots
	evidence.Submitted = metric.Submitted
	evidence.ExpectedReceives = metric.ExpectedReceives
	evidence.DeliveredUnique = metric.DeliveredUnique
	evidence.EligibleDeliveredUnique = eligibleDeliveredUnique(result, metric, definition)
	evidence.Duplicates = metric.Duplicates
	evidence.Corruption = corruption
	evidence.LossEvidenceSHA256 = lossHash
	evidence.MetricsArtifacts = append([]MetricsArtifactDigest(nil), metricsArtifacts...)
	if coverageReasons := validateQdiscTrafficCoverage(request.Config, definition, evidence.EligibleDeliveredUnique, evidence.Loss); len(coverageReasons) != 0 {
		extraction.InvalidReasons = dedupeSorted(coverageReasons)
		return extraction, nil
	}
	extraction.Evidence = &evidence
	return extraction, nil
}

func baseEvidence(request ExtractRequest, definition CaseDefinition, resultSHA string,
	treatment extractionTreatmentEvidence, offloadsDisabled bool, loss LossContractEvidence, sutFailures []string,
) EvidenceRun {
	mappingSHA := MappingSHA256(request.Mapping)
	return EvidenceRun{
		CaseID:                definition.ID,
		Transport:             request.ExpectedTransport,
		AttemptNumber:         request.AttemptNumber,
		RunIdentity:           request.ExpectedRunIdentity,
		AcquisitionID:         request.AcquisitionID,
		ResultSHA256:          resultSHA,
		MappingSHA256:         mappingSHA,
		EndpointMappingSHA256: endpointMappingSHA256(request.Mapping, definition, mappingSHA),
		CaseIdentity:          request.ExpectedCaseIdentity,
		MeasurementValid:      true,
		SUTFailureReasons:     append([]string(nil), sutFailures...),
		Scenario:              run.ScenarioEnvironmentBaseline,
		TrafficClass:          definition.Class,
		ClassExclusive:        true,
		Echo:                  true,
		// Endpoint descriptions and executable hashes bind the fixed body
		// generator; invalid_payload then verifies every received body.
		PayloadPatternVerified:  treatment.payloadPatternVerified,
		WireCompressionDisabled: treatment.wireCompressionDisabled,
		ServerCoalescing:        treatment.serverCoalescing,
		ClientCoalescing:        treatment.clientCoalescing,
		OffloadsDisabled:        offloadsDisabled,
		Loss:                    loss,
	}
}

func validateExtractionBindings(request ExtractRequest, result run.Result, root, resultSHA string, definition CaseDefinition) []string {
	var reasons []string
	if result.Version != resultVersion {
		reasons = append(reasons, fmt.Sprintf("result version=%d, want %d", result.Version, resultVersion))
	}
	if !run.IsSafeName(request.ExpectedTransport) {
		reasons = append(reasons, "expected transport is not a path-safe name")
	}
	if result.Transport != request.ExpectedTransport {
		reasons = append(reasons, fmt.Sprintf("result transport=%q, want %q", result.Transport, request.ExpectedTransport))
	}
	if result.Config.Transport != request.ExpectedTransport {
		reasons = append(reasons, fmt.Sprintf("config transport=%q, want %q", result.Config.Transport, request.ExpectedTransport))
	}
	if !isSHA256(request.ExpectedResultSHA256) {
		reasons = append(reasons, "expected_result_sha256 is not a SHA-256 hex digest")
	} else if request.ExpectedResultSHA256 != resultSHA {
		reasons = append(reasons, fmt.Sprintf("result_sha256=%q, want ledger hash %q", resultSHA, request.ExpectedResultSHA256))
	}
	if request.ExpectedRunConfig == nil {
		reasons = append(reasons, "expected run config is missing")
	} else {
		if !reflect.DeepEqual(*request.ExpectedRunConfig, result.Config) {
			reasons = append(reasons, "result config does not match the planned run config")
		}
		if request.ExpectedRunConfig.Transport != request.ExpectedTransport {
			reasons = append(reasons, "planned run config transport does not match expected transport")
		}
	}
	if filepath.IsAbs(result.Config.OutputDir) {
		resolved, err := filepath.EvalSymlinks(filepath.Clean(result.Config.OutputDir))
		if err != nil || resolved != root {
			reasons = append(reasons, "config output_dir does not resolve to the extracted run directory")
		}
	}
	if !isSHA256(request.ExpectedRunIdentity) {
		reasons = append(reasons, "expected_run_identity is not a SHA-256 hex digest")
	} else if result.Treatment != nil {
		if actual := recordedRunIdentity(result); request.ExpectedRunIdentity != actual {
			reasons = append(reasons, fmt.Sprintf("run_identity=%q, want recorded treatment identity %q", request.ExpectedRunIdentity, actual))
		}
	}
	if !isSHA256(request.AcquisitionID) {
		reasons = append(reasons, "acquisition_id is not a SHA-256 hex digest")
	}
	if request.AttemptNumber < 1 || request.AttemptNumber > request.Config.MaxAttemptsPerCase {
		reasons = append(reasons, fmt.Sprintf("attempt_number=%d, want 1..%d", request.AttemptNumber, request.Config.MaxAttemptsPerCase))
	}
	if invalid := request.Config.Validate(); len(invalid) != 0 {
		reasons = appendPrefixed(reasons, "conformance config: ", invalid)
	}
	if invalid := run.ValidateClassMappingRecord(request.Mapping); len(invalid) != 0 {
		reasons = appendPrefixed(reasons, "planned class mapping: ", invalid)
	}
	mappingSHA := MappingSHA256(request.Mapping)
	probeIdentity := ProbeIdentity(request.Config, request.ExpectedTransport, mappingSHA)
	caseIdentity := CaseIdentity(probeIdentity, definition)
	if expected := AcquisitionIdentity(caseIdentity, request.AttemptNumber, request.ExpectedRunIdentity); request.AcquisitionID != expected {
		reasons = append(reasons, fmt.Sprintf("acquisition_id=%q, want planned attempt identity %q", request.AcquisitionID, expected))
	}
	if !isSHA256(request.ExpectedProbeIdentity) || request.ExpectedProbeIdentity != probeIdentity {
		reasons = append(reasons, fmt.Sprintf("probe_identity=%q, want %q", request.ExpectedProbeIdentity, probeIdentity))
	}
	if !isSHA256(request.ExpectedCaseIdentity) || request.ExpectedCaseIdentity != caseIdentity {
		reasons = append(reasons, fmt.Sprintf("case_identity=%q, want %q", request.ExpectedCaseIdentity, caseIdentity))
	}
	return reasons
}

func recordedRunIdentity(result run.Result) string {
	canonical := result.Config
	canonical.OutputDir = ""
	canonical.NetemGateOff = false
	type commandIdentity struct {
		Command run.CommandConfig `json:"command"`
		SHA256  string            `json:"sha256,omitempty"`
	}
	record := struct {
		Config             run.RunConfig   `json:"config"`
		Server             commandIdentity `json:"server"`
		Client             commandIdentity `json:"client"`
		OrchestratorSHA256 string          `json:"orchestrator_sha256,omitempty"`
		EnvironmentSHA256  string          `json:"environment_sha256"`
	}{Config: canonical}
	if result.Treatment != nil {
		record.Server = commandIdentity{canonical.ServerCommand, result.Treatment.Server.SHA256}
		record.Client = commandIdentity{canonical.ClientCommand, result.Treatment.Client.SHA256}
		record.OrchestratorSHA256 = result.Treatment.OrchestratorSHA256
		record.EnvironmentSHA256 = result.Treatment.EnvironmentSHA256
	}
	return run.HashValue(record)
}

type extractionTreatmentEvidence struct {
	payloadPatternVerified  bool
	wireCompressionDisabled bool
	serverCoalescing        string
	clientCoalescing        string
}

func validateExtractionTreatment(result run.Result, planned run.ClassMappingRecord) (extractionTreatmentEvidence, []string) {
	var evidence extractionTreatmentEvidence
	invalid, unsupported := run.ValidateScenarioTreatmentContract(result.Treatment, result.Config)
	reasons := appendPrefixed(nil, "treatment contract: ", invalid)
	reasons = appendPrefixed(reasons, "treatment unsupported: ", unsupported)
	if result.Treatment == nil {
		return evidence, reasons
	}
	if run.HashValue(result.Treatment.ClassMapping) != run.HashValue(planned) {
		reasons = append(reasons, "treatment class mapping does not match the planned mapping")
	}
	evidence.payloadPatternVerified = true
	evidence.wireCompressionDisabled = true
	for _, endpoint := range []struct {
		name        string
		description json.RawMessage
	}{{"server", result.Treatment.Server.Description}, {"client", result.Treatment.Client.Description}} {
		var advertised struct {
			Coalescing      string `json:"coalescing"`
			PayloadPattern  string `json:"payload_pattern"`
			WireCompression string `json:"wire_compression"`
		}
		if err := json.Unmarshal(endpoint.description, &advertised); err != nil {
			reasons = append(reasons, endpoint.name+" --describe packet-exposure fields cannot be decoded")
			evidence.payloadPatternVerified = false
			evidence.wireCompressionDisabled = false
			continue
		}
		if advertised.PayloadPattern != "splitmix64-v1" {
			reasons = append(reasons, fmt.Sprintf("%s --describe payload_pattern=%q, want splitmix64-v1", endpoint.name, advertised.PayloadPattern))
			evidence.payloadPatternVerified = false
		}
		if advertised.WireCompression != "none" {
			reasons = append(reasons, fmt.Sprintf("%s --describe wire_compression=%q, want none", endpoint.name, advertised.WireCompression))
			evidence.wireCompressionDisabled = false
		}
		switch endpoint.name {
		case "server":
			evidence.serverCoalescing = advertised.Coalescing
		case "client":
			evidence.clientCoalescing = advertised.Coalescing
		}
	}
	return evidence, reasons
}

func validateProbeScenario(cfg run.RunConfig, conformance Config, definition CaseDefinition) (*run.TrafficClassSpec, uint8, []string) {
	var reasons []string
	if cfg.Scenario == nil {
		return nil, 0, []string{"scenario is missing"}
	}
	if err := cfg.Scenario.Validate(); err != nil {
		reasons = append(reasons, "scenario contract: "+err.Error())
	}
	if cfg.Scenario.Kind != run.ScenarioEnvironmentBaseline {
		reasons = append(reasons, fmt.Sprintf("scenario=%q, want environment_baseline", cfg.Scenario.Kind))
	}
	if cfg.Scenario.ClientInput == nil {
		return nil, 0, append(reasons, "environment_baseline client_input is missing")
	}
	traffic := cfg.Scenario.ClientInput
	if traffic.TrafficID != run.TrafficIDClientInput {
		reasons = append(reasons, fmt.Sprintf("client_input traffic_id=%d, want %d", traffic.TrafficID, run.TrafficIDClientInput))
	}
	selected, opposing := &traffic.LossTolerant, traffic.MustDeliver
	if definition.Class == run.ClassMustDeliver {
		selected, opposing = &traffic.MustDeliver, traffic.LossTolerant
	}
	if opposing != (run.TrafficClassSpec{}) {
		reasons = append(reasons, fmt.Sprintf("opposing traffic class is enabled or has non-zero settings: %+v", opposing))
	}
	wantSelected := run.TrafficClassSpec{RateHz: float64(conformance.RateHz), PayloadBytes: int(conformance.PayloadBytes)}
	if *selected != wantSelected {
		reasons = append(reasons, fmt.Sprintf("selected class settings=%+v, want fixed probe settings=%+v", *selected, wantSelected))
	}
	if cfg.TotalConns != 1 || cfg.ClientProcs != 1 {
		reasons = append(reasons, fmt.Sprintf("probe topology client_procs=%d total_conns=%d, want 1/1", cfg.ClientProcs, cfg.TotalConns))
	}
	if cfg.Netem == nil {
		reasons = append(reasons, "probe netem namespace treatment is missing")
	} else if cfg.Netem.LinkMTUBytes != int(conformance.LinkMTUBytes) {
		reasons = append(reasons, fmt.Sprintf("link_mtu_bytes=%d, want %d", cfg.Netem.LinkMTUBytes, conformance.LinkMTUBytes))
	}
	if cfg.AttemptedThreshold != 1 {
		reasons = append(reasons, fmt.Sprintf("attempted_threshold=%g, want 1 for exact submission", cfg.AttemptedThreshold))
	}
	for name, values := range map[string]struct{ got, want int64 }{
		"warmup":   {int64(cfg.Warmup.Duration), int64(conformance.WarmupNS)},
		"duration": {int64(cfg.Duration.Duration), int64(conformance.DurationNS)},
		"drain":    {int64(cfg.Drain.Duration), int64(conformance.DrainNS)},
	} {
		if values.got != values.want {
			reasons = append(reasons, fmt.Sprintf("%s_ns=%d, want %d", name, values.got, values.want))
		}
	}
	if cfg.SteadyWarmup {
		reasons = append(reasons, "steady_warmup must be disabled for the fixed conformance schedule")
	}
	if cfg.NetemGateOff {
		reasons = append(reasons, "netem gate must remain enabled for conformance acquisition")
	}
	if cfg.DeadlineNS != 0 {
		reasons = append(reasons, "legacy deadline_ns must be zero for the scenario probe")
	}
	return selected, traffic.TrafficID, reasons
}

type extractedProcesses struct {
	serverPath  string
	clientPaths []string
	clientConns []int
}

func validateProbeProcesses(result run.Result, transport string) (extractedProcesses, []string, []string) {
	var out extractedProcesses
	var identityReasons, completenessReasons []string
	if len(result.Processes) != 2 {
		completenessReasons = append(completenessReasons, fmt.Sprintf("process records=%d, want one server and one client", len(result.Processes)))
	}
	seenServer, seenClient := false, false
	for _, process := range result.Processes {
		switch process.Role {
		case "server":
			if seenServer || process.ProcIndex != -1 {
				identityReasons = append(identityReasons, "server process record is duplicated or has non-canonical proc_index")
			}
			seenServer = true
			out.serverPath = process.MetricsOut
		case "client":
			if seenClient || process.ProcIndex != 0 || process.Conns != 1 || process.OriginIDStart != 0 || process.OriginIDEnd != 1 {
				identityReasons = append(identityReasons, "client process record does not prove one canonical connection partition")
			}
			seenClient = true
			out.clientPaths = append(out.clientPaths, process.MetricsOut)
			out.clientConns = append(out.clientConns, process.Conns)
		default:
			identityReasons = append(identityReasons, fmt.Sprintf("unknown process role %q", process.Role))
		}
		if len(process.Command) == 0 {
			identityReasons = append(identityReasons, fmt.Sprintf("%s process command is missing", process.Role))
		} else if err := run.ValidateRecordedProcessCommand(result.Config, process, true); err != nil {
			identityReasons = append(identityReasons, fmt.Sprintf("%s process command: %v", process.Role, err))
		}
		if process.PID <= 0 {
			identityReasons = append(identityReasons, fmt.Sprintf("%s process PID=%d is invalid", process.Role, process.PID))
		}
	}
	if !seenServer || !seenClient {
		completenessReasons = append(completenessReasons, "process records do not contain exactly one server and one client")
	}
	return out, identityReasons, completenessReasons
}

func validateProbeParticipants(result run.Result) (uint64, []string) {
	if result.Control == nil {
		return 0, []string{"control result is missing"}
	}
	var reasons []string
	if !result.Control.Valid || len(result.Control.InvalidReasons) != 0 {
		reasons = append(reasons, "control result is invalid")
	}
	if len(result.Control.Participants) != 2 {
		reasons = append(reasons, fmt.Sprintf("control participants=%d, want one server and one client", len(result.Control.Participants)))
	}
	if result.Control.Schedule.Type != control.TypeSchedule || result.Control.Schedule.StartAtNS <= 0 {
		reasons = append(reasons, "control schedule is not the fixed positive schedule contract")
	}
	if result.Control.Schedule.StopAtNS-result.Control.Schedule.StartAtNS != int64(result.Config.Duration.Duration) ||
		result.Control.Schedule.DrainUntilNS-result.Control.Schedule.StopAtNS != int64(result.Config.Drain.Duration) {
		reasons = append(reasons, "control schedule does not match configured duration/drain")
	}
	seen := map[string]bool{}
	seenConnID := map[int]bool{}
	processByRole := map[string]run.ProcessResult{}
	for _, process := range result.Processes {
		processByRole[process.Role] = process
	}
	var corruption uint64
	for _, participant := range result.Control.Participants {
		role := participant.Hello.Role
		if role != "server" && role != "client" {
			reasons = append(reasons, fmt.Sprintf("participant has unknown role %q", role))
			continue
		}
		if seen[role] {
			reasons = append(reasons, "duplicate "+role+" participant")
		}
		seen[role] = true
		if seenConnID[participant.ConnID] {
			reasons = append(reasons, fmt.Sprintf("duplicate participant conn_id=%d", participant.ConnID))
		}
		seenConnID[participant.ConnID] = true
		// Endpoint control clients use proc_index=0 for the single server and
		// the single client. The runner's ProcessResult uses -1 only for the
		// server process-table slot; that internal sentinel is not sent on the
		// control protocol.
		wantIndex := 0
		expectedProcess, processOK := processByRole[role]
		if participant.Hello.Type != control.TypeHello || participant.Hello.ProcIndex != wantIndex ||
			participant.Hello.Transport != result.Transport || !processOK || participant.Hello.PID != expectedProcess.PID {
			reasons = append(reasons, role+" participant identity does not match the run")
		}
		if !participant.ReadyReceived || !participant.AckReceived || !participant.DoneReceived {
			reasons = append(reasons, role+" participant did not complete every control stage")
		}
		if participant.Ready.Type != control.TypeReady || participant.SchedAck.Type != control.TypeSchedAck ||
			participant.Done.Type != control.TypeDone || participant.SchedAck.MarginNS < 0 {
			reasons = append(reasons, role+" participant control messages are malformed")
		}
		// The server becomes ready before clients connect, so its ready message
		// carries zero connections. The single client owns the probe connection.
		wantConns := 1
		if role == "server" {
			wantConns = 0
		}
		if participant.Ready.Conns != wantConns {
			reasons = append(reasons, fmt.Sprintf("%s ready conns=%d, want %d", role, participant.Ready.Conns, wantConns))
		}
		var stats struct {
			InvalidPayload *uint64 `json:"invalid_payload"`
		}
		if err := json.Unmarshal(participant.Done.Stats, &stats); err != nil || stats.InvalidPayload == nil {
			reasons = append(reasons, role+" done.stats does not contain an integer invalid_payload counter")
			continue
		}
		if math.MaxUint64-corruption < *stats.InvalidPayload {
			reasons = append(reasons, "invalid_payload counter sum overflows uint64")
			continue
		}
		corruption += *stats.InvalidPayload
	}
	if !seen["server"] || !seen["client"] {
		reasons = append(reasons, "control result lacks a server or client participant")
	}
	return corruption, reasons
}

type metricsArtifactSnapshot struct {
	server  run.MetricsArtifactData
	clients []run.MetricsArtifactData
	digests []MetricsArtifactDigest
}

func extractMergedMetrics(root string, rootFD int, result run.Result, processes extractedProcesses) (*run.MergedMetrics, []MetricsArtifactDigest, error) {
	snapshot, err := snapshotMetricsArtifacts(root, rootFD, result.Config.OutputDir, processes)
	if err != nil {
		return nil, nil, err
	}
	merged, err := validateMetricsSnapshot(snapshot, result, processes)
	if err != nil {
		return nil, nil, err
	}
	return merged, append([]MetricsArtifactDigest(nil), snapshot.digests...), nil
}

func snapshotMetricsArtifacts(root string, rootFD int, declaredOutputDir string, processes extractedProcesses) (metricsArtifactSnapshot, error) {
	var snapshot metricsArtifactSnapshot
	serverRelative, err := metricsRelativeReference(root, declaredOutputDir, processes.serverPath)
	if err != nil {
		return snapshot, fmt.Errorf("server metrics path: %w", err)
	}
	clientRelative := make([]string, 0, len(processes.clientPaths))
	seen := map[string]bool{serverRelative: true}
	for _, reference := range processes.clientPaths {
		relative, err := metricsRelativeReference(root, declaredOutputDir, reference)
		if err != nil {
			return snapshot, fmt.Errorf("client metrics path: %w", err)
		}
		if seen[relative] {
			return snapshot, fmt.Errorf("duplicate metrics path %s", relative)
		}
		seen[relative] = true
		clientRelative = append(clientRelative, relative)
	}

	server, digest, err := readMetricsArtifactBeneath(rootFD, serverRelative)
	if err != nil {
		return snapshot, fmt.Errorf("read metrics %s: %w", serverRelative, err)
	}
	snapshot.server = server
	snapshot.digests = append(snapshot.digests, digest)
	for _, relative := range clientRelative {
		client, digest, err := readMetricsArtifactBeneath(rootFD, relative)
		if err != nil {
			return metricsArtifactSnapshot{}, fmt.Errorf("read metrics %s: %w", relative, err)
		}
		snapshot.clients = append(snapshot.clients, client)
		snapshot.digests = append(snapshot.digests, digest)
	}
	return snapshot, nil
}

func validateMetricsSnapshot(snapshot metricsArtifactSnapshot, result run.Result, processes extractedProcesses) (*run.MergedMetrics, error) {
	merged, err := run.MergeMetricsData(snapshot.clients, result.Config.TotalConns)
	if err != nil {
		return nil, err
	}
	if err := run.ValidateMergedMetricsConsistency(merged); err != nil {
		return nil, fmt.Errorf("re-merged consistency: %w", err)
	}
	if err := run.ValidateScenarioMetricsData(snapshot.server, snapshot.clients, processes.clientConns,
		result.Config.TotalConns, result.Config.Duration.Duration, result.Config.StalenessPeriodNS,
		*result.Config.Scenario, merged); err != nil {
		return nil, fmt.Errorf("scenario metrics contract: %w", err)
	}
	if result.Metrics == nil {
		return nil, fmt.Errorf("result.json merged metrics are missing")
	}
	if err := run.ValidateMergedMetricsConsistency(result.Metrics); err != nil {
		return nil, fmt.Errorf("persisted merged consistency: %w", err)
	}
	if run.HashValue(merged) != run.HashValue(result.Metrics) {
		return nil, fmt.Errorf("persisted merged metrics do not match raw process metrics")
	}
	return merged, nil
}

func extractExclusiveMetric(merged *run.MergedMetrics, metric run.TrafficAggregate, reasons []string, trafficID uint8, class string) (run.TrafficAggregate, []string) {
	if len(merged.Traffic) != 1 {
		reasons = append(reasons, fmt.Sprintf("merged traffic series=%d, want exactly one class-exclusive series", len(merged.Traffic)))
	}
	selected, ok := merged.TrafficMetric(trafficID, run.DirectionRoomRelay, class)
	if !ok {
		reasons = append(reasons, fmt.Sprintf("merged metrics lack traffic_id=%d direction=%s class=%s", trafficID, run.DirectionRoomRelay, class))
		return metric, reasons
	}
	metric = selected
	if metric.SlotsBroadcast != 0 {
		reasons = append(reasons, fmt.Sprintf("slots_broadcast=%d, echo probe requires zero", metric.SlotsBroadcast))
	}
	if metric.ExpectedReceives != metric.Slots {
		reasons = append(reasons, fmt.Sprintf("expected_receives=%d, want echo slots=%d", metric.ExpectedReceives, metric.Slots))
	}
	if metric.LatencySchedNS.Count != metric.DeliveredUnique || metric.LatencySendNS.Count != metric.DeliveredUnique {
		reasons = append(reasons, "latency histogram counts do not account for every unique delivery")
	}
	opposing := run.ClassLossTolerant
	if class == run.ClassLossTolerant {
		opposing = run.ClassMustDeliver
	}
	if aggregate, ok := merged.Classes[opposing]; !ok || aggregate.ClassCounts != (run.ClassCounts{}) {
		reasons = append(reasons, "opposing class aggregate is non-zero or missing")
	}
	return metric, reasons
}

func extractStaticLossTreatment(result run.Result, config Config, definition CaseDefinition) (LossContractEvidence, []string) {
	if result.Config.Netem == nil || result.Netem == nil || !result.Netem.Enabled {
		return LossContractEvidence{}, []string{"probe lacks an enabled netns treatment record"}
	}
	netem := result.Config.Netem
	if definition.Clean {
		loss := LossContractEvidence{Mode: LossModeNone}
		var reasons []string
		if netem.ClientEgress != (netops.Netem{}) || netem.ServerEgress != (netops.Netem{}) {
			reasons = append(reasons, "clean probe contains non-zero netem impairment parameters")
		}
		reasons = append(reasons, validateStaticLossContract(config, definition, loss)...)
		return loss, reasons
	}
	loss := LossContractEvidence{
		Mode:                    LossModeRandomNetem,
		ClientEgressLossPercent: netem.ClientEgress.LossPercent,
		ServerEgressLossPercent: netem.ServerEgress.LossPercent,
		LossSeed:                netem.ClientEgress.LossSeed + netem.ServerEgress.LossSeed,
		LossBurstLength:         netem.ClientEgress.LossBurstLen + netem.ServerEgress.LossBurstLen,
		RandomLossOnly:          randomLossOnly(netem.ClientEgress) && randomLossOnly(netem.ServerEgress),
	}
	return loss, validateStaticLossContract(config, definition, loss)
}

func pairSpecForRegime(regime *run.NetemRegime) netops.PairSpec {
	pair := netops.DefaultPair(regime.Prefix)
	if regime.ServerNS != "" {
		pair.ServerNS = regime.ServerNS
	}
	if regime.ClientNS != "" {
		pair.ClientNS = regime.ClientNS
	}
	if regime.ServerVeth != "" {
		pair.ServerVeth = regime.ServerVeth
	}
	if regime.ClientVeth != "" {
		pair.ClientVeth = regime.ClientVeth
	}
	if regime.ServerAddrCIDR != "" {
		pair.ServerAddrCIDR = regime.ServerAddrCIDR
	}
	if regime.ClientAddrCIDR != "" {
		pair.ClientAddrCIDR = regime.ClientAddrCIDR
	}
	pair.DisableOffloads = regime.DisableOffloads
	pair.ServerEgress = regime.ServerEgress
	pair.ClientEgress = regime.ClientEgress
	return pair
}

func extractLossEvidence(result run.Result, config Config, definition CaseDefinition, metric run.TrafficAggregate) (LossContractEvidence, string, []string) {
	if definition.Clean {
		if result.Config.Netem == nil {
			return LossContractEvidence{}, "", []string{"clean probe is missing the common netns/offload treatment"}
		}
		if result.Config.Netem.ClientEgress != (netops.Netem{}) || result.Config.Netem.ServerEgress != (netops.Netem{}) {
			return LossContractEvidence{}, "", []string{"clean probe contains non-zero netem impairment parameters"}
		}
		if result.Netem == nil || !result.Netem.Enabled {
			return LossContractEvidence{}, "", []string{"clean probe lacks netns treatment evidence"}
		}
		if result.Netem.LossEvidence != nil {
			return LossContractEvidence{}, "", []string{"clean probe unexpectedly contains loss evidence"}
		}
		loss := LossContractEvidence{Mode: LossModeNone}
		reasons := validateStaticLossContract(config, definition, loss)
		reasons = append(reasons, validateDynamicLossEvidence(definition, loss)...)
		return loss, "", reasons
	}
	if result.Netem == nil || result.Config.Netem == nil {
		return LossContractEvidence{}, "", []string{"loss probe netem evidence is missing"}
	}
	invalid := run.ValidateNetemLossEvidence(&result.Config, result.Control, result.Netem.LossEvidence)
	if len(invalid) != 0 {
		return LossContractEvidence{}, "", appendPrefixed(nil, "", invalid)
	}
	evidence := result.Netem.LossEvidence
	if evidence == nil || evidence.Before == nil || evidence.After == nil {
		return LossContractEvidence{}, "", []string{"loss probe qdisc evidence or snapshots are missing"}
	}
	computed := netops.DeltaQdiscPair(*evidence.Before, *evidence.After)
	if computed.ServerEgress == nil || computed.ClientEgress == nil {
		return LossContractEvidence{}, "", []string{"validated qdisc evidence has no directional deltas"}
	}
	netem := result.Config.Netem
	randomOnly := randomLossOnly(netem.ServerEgress) && randomLossOnly(netem.ClientEgress) &&
		computed.ServerEgress.Requeues == 0 && computed.ClientEgress.Requeues == 0
	queueOverflow := computed.ServerEgress.Overlimits + computed.ClientEgress.Overlimits
	if queueOverflow < computed.ServerEgress.Overlimits {
		return LossContractEvidence{}, "", []string{"qdisc overlimit counter sum overflows uint64"}
	}
	loss := LossContractEvidence{
		Mode:                    LossModeRandomNetem,
		Supported:               evidence.Supported,
		Scope:                   evidence.Scope,
		ClientEgressLossPercent: netem.ClientEgress.LossPercent,
		ServerEgressLossPercent: netem.ServerEgress.LossPercent,
		ClientEgressDropped:     computed.ClientEgress.Dropped,
		ServerEgressDropped:     computed.ServerEgress.Dropped,
		ClientEgressSentBytes:   computed.ClientEgress.SentBytes,
		ClientEgressSentPackets: computed.ClientEgress.SentPackets,
		ServerEgressSentBytes:   computed.ServerEgress.SentBytes,
		ServerEgressSentPackets: computed.ServerEgress.SentPackets,
		LossSeed:                netem.ClientEgress.LossSeed + netem.ServerEgress.LossSeed,
		LossBurstLength:         netem.ClientEgress.LossBurstLen + netem.ServerEgress.LossBurstLen,
		RandomLossOnly:          randomOnly,
		QueueOverflowDrops:      queueOverflow,
	}
	var reasons []string
	reasons = append(reasons, validateStaticLossContract(config, definition, loss)...)
	reasons = append(reasons, validateDynamicLossEvidence(definition, loss)...)
	if metric.Slots == 0 {
		reasons = append(reasons, "loss probe has no metric slots")
	}
	return loss, run.HashValue(evidence), reasons
}

func randomLossOnly(value netops.Netem) bool {
	return value.DelayMS == 0 && value.JitterMS == 0 && value.Rate == "" && value.LossBurstLen == 0 &&
		value.LossSeed == 0 && value.TraceBits == 0 && value.Limit == 0
}

func eligibleDeliveredUnique(result run.Result, metric run.TrafficAggregate, definition CaseDefinition) uint64 {
	if definition.Clean {
		return metric.DeliveredUnique
	}
	if result.Control == nil || result.Netem == nil || result.Netem.LossEvidence == nil ||
		result.Netem.LossEvidence.Before == nil || result.Netem.LossEvidence.After == nil || metric.Slots == 0 {
		return 0
	}
	maxLatency := observedHistogramUpperBound(metric.LatencySchedNS, result.Metrics.Histogram)
	if maxLatency == 0 || maxLatency > math.MaxInt64 {
		return 0
	}
	schedule := result.Control.Schedule
	before := result.Netem.LossEvidence.Before.CaptureFinishNS
	after := result.Netem.LossEvidence.After.CaptureStartNS - int64(maxLatency)
	if before >= after || before < schedule.StartAtNS || after > schedule.StopAtNS {
		return 0
	}
	interval := int64(1_000_000_000 / result.Config.Scenario.ClientInput.LossTolerant.RateHz)
	if definition.Class == run.ClassMustDeliver {
		interval = int64(1_000_000_000 / result.Config.Scenario.ClientInput.MustDeliver.RateHz)
	}
	if interval <= 0 {
		return 0
	}
	prefix := ceilDivPositive(before-schedule.StartAtNS, interval) + 1
	suffix := ceilDivPositive(schedule.StopAtNS-after, interval) + 1
	excluded := uint64(prefix + suffix)
	if excluded > metric.Slots {
		excluded = metric.Slots
	}
	if metric.DeliveredUnique <= excluded {
		return 0
	}
	return metric.DeliveredUnique - excluded
}

func observedHistogramUpperBound(histogram run.Histogram, layout run.HistogramLayout) uint64 {
	for index := len(histogram.Bins) - 1; index >= 0; index-- {
		if histogram.Bins[index] != 0 {
			return histogramBinUpperNS(layout, index, len(histogram.Bins))
		}
	}
	return 0
}

func histogramBinUpperNS(layout run.HistogramLayout, index, bins int) uint64 {
	if index < 0 || bins <= 0 || uint(index) >= uint(bins) || layout.Subbins == 0 {
		return 0
	}
	if index == 0 {
		return layout.MinNS
	}
	if index == bins-1 {
		return layout.MaxNS
	}
	exponent := uint(index) / layout.Subbins
	sub := uint(index) % layout.Subbins
	if exponent >= 63 || layout.MinNS > math.MaxUint64>>exponent {
		return layout.MaxNS
	}
	low := layout.MinNS << exponent
	increment := (low*uint64(sub+1) + uint64(layout.Subbins) - 1) / uint64(layout.Subbins)
	if math.MaxUint64-low < increment || low+increment > layout.MaxNS {
		return layout.MaxNS
	}
	return low + increment
}

func ceilDivPositive(value, divisor int64) int64 {
	if value <= 0 {
		return 0
	}
	return 1 + (value-1)/divisor
}

func processFailureReasons(processes []run.ProcessResult) []string {
	var reasons []string
	for _, process := range processes {
		if !process.Exited {
			reasons = append(reasons, fmt.Sprintf("%s proc_index=%d did not exit", process.Role, process.ProcIndex))
		} else if process.ExitCode != 0 {
			reason := fmt.Sprintf("%s proc_index=%d exit_code=%d", process.Role, process.ProcIndex, process.ExitCode)
			if process.Error != "" {
				reason += ": " + process.Error
			}
			reasons = append(reasons, reason)
		}
	}
	return dedupeSorted(reasons)
}

func lifecycleSUTFailureReasons(result run.Result) []string {
	participants := map[string]control.Participant{}
	if result.Control != nil {
		for _, participant := range result.Control.Participants {
			participants[participant.Hello.Role] = participant
		}
	}
	var reasons []string
	for _, process := range result.Processes {
		if !process.Exited || process.ExitCode != 0 {
			continue
		}
		participant, present := participants[process.Role]
		// Server ProcessResult uses proc_index=-1 while its control hello uses 0,
		// so role and PID are the cross-record identity. A different PID is
		// contradictory evidence, not proof that this process exited early; the
		// normal participant validator rejects it.
		if present && participant.Hello.PID != process.PID {
			continue
		}
		if !present || !participant.DoneReceived {
			reasons = append(reasons, fmt.Sprintf("%s proc_index=%d pid=%d exited before completing the control lifecycle",
				process.Role, process.ProcIndex, process.PID))
		}
	}
	return reasons
}

func networkSUTFailureReasons(result run.Result) (failures, invalid []string) {
	if result.Netem == nil {
		return nil, nil
	}
	for _, item := range []struct {
		name          string
		before, after netops.UDPStats
		stored        netops.UDPStats
	}{
		{"client", result.Netem.UDPBefore, result.Netem.UDPAfter, result.Netem.UDPDelta},
		{"server", result.Netem.ServerUDPBefore, result.Netem.ServerUDPAfter, result.Netem.ServerUDPDelta},
	} {
		if item.after.InErrors < item.before.InErrors || item.after.RcvbufErrors < item.before.RcvbufErrors {
			invalid = append(invalid, item.name+" UDP counters regressed")
			continue
		}
		computed := netops.DeltaUDPStats(item.before, item.after)
		if item.stored != computed {
			invalid = append(invalid, fmt.Sprintf("%s stored UDP delta=%+v does not match before/after=%+v", item.name, item.stored, computed))
			continue
		}
		if computed.InErrors != 0 || computed.RcvbufErrors != 0 {
			failures = append(failures, fmt.Sprintf("%s UDP receive drops: InErrors=%d RcvbufErrors=%d",
				item.name, computed.InErrors, computed.RcvbufErrors))
		}
	}
	return dedupeSorted(failures), dedupeSorted(invalid)
}

func independentResultInvalidReasons(reasons []string) []string {
	var independent []string
	for _, reason := range reasons {
		consequence := false
		for _, fragment := range []string{
			"control result is missing", "control barrier failed", "merged metrics are missing",
			"metrics contract:", "process exited before control barrier", "server exited before becoming ready",
			"netem loss evidence:", "client netns UDP drop delta non-zero", "done.stats",
			" exit_code=", " did not exit", "timed out waiting", "attempted_ratio=",
		} {
			if strings.Contains(reason, fragment) {
				consequence = true
				break
			}
		}
		if !consequence {
			independent = append(independent, reason)
		}
	}
	return dedupeSorted(independent)
}

func secureRunRoot(path string) (string, error) {
	if strings.TrimSpace(path) == "" {
		return "", fmt.Errorf("run directory is empty")
	}
	absolute, err := filepath.Abs(path)
	if err != nil {
		return "", fmt.Errorf("absolute run directory: %w", err)
	}
	root, err := filepath.EvalSymlinks(absolute)
	if err != nil {
		return "", fmt.Errorf("resolve run directory: %w", err)
	}
	info, err := os.Stat(root)
	if err != nil {
		return "", fmt.Errorf("stat run directory: %w", err)
	}
	if !info.IsDir() {
		return "", fmt.Errorf("run directory is not a directory")
	}
	return filepath.Clean(root), nil
}

func metricsRelativeReference(root, declaredOutputDir, reference string) (string, error) {
	if strings.TrimSpace(reference) == "" {
		return "", fmt.Errorf("reference is empty")
	}
	cleanReference := filepath.Clean(reference)
	var relative string
	if filepath.IsAbs(cleanReference) {
		var err error
		relative, err = filepath.Rel(root, cleanReference)
		if err != nil {
			return "", err
		}
	} else {
		relative = cleanReference
		cleanOutput := filepath.Clean(declaredOutputDir)
		if declaredOutputDir != "" && !filepath.IsAbs(cleanOutput) {
			if rel, err := filepath.Rel(cleanOutput, cleanReference); err == nil && pathWithinRoot(rel) {
				relative = rel
			}
		}
	}
	if !pathWithinRoot(relative) {
		return "", fmt.Errorf("reference %q escapes the run directory", reference)
	}
	relative = filepath.Clean(relative)
	portable := filepath.ToSlash(relative)
	parts := strings.Split(portable, "/")
	if len(parts) < 2 || parts[0] != "metrics" {
		return "", fmt.Errorf("reference %q is outside the metrics artifact directory", reference)
	}
	for _, part := range parts {
		if part == "" || part == "." || part == ".." {
			return "", fmt.Errorf("reference %q has an unsafe path component", reference)
		}
	}
	return portable, nil
}

func pathWithinRoot(relative string) bool {
	return relative != "." && relative != "" && !filepath.IsAbs(relative) && relative != ".." &&
		!strings.HasPrefix(relative, ".."+string(filepath.Separator))
}

func readMetricsArtifactBeneath(rootFD int, relative string) (run.MetricsArtifactData, MetricsArtifactDigest, error) {
	data, err := readRegularBeneath(rootFD, relative, maxMetricsFileBytes)
	if err != nil {
		return run.MetricsArtifactData{}, MetricsArtifactDigest{}, err
	}
	artifact := run.NewMetricsArtifactData(relative, data)
	digest := MetricsArtifactDigest{
		RelativePath: relative,
		SizeBytes:    uint64(len(data)),
		SHA256:       artifact.SHA256(),
	}
	return artifact, digest, nil
}

func readRegularBeneath(rootFD int, relative string, limit int64) ([]byte, error) {
	fd, err := openRegularBeneath(rootFD, relative)
	if err != nil {
		return nil, err
	}
	file := os.NewFile(uintptr(fd), relative)
	if file == nil {
		unix.Close(fd)
		return nil, fmt.Errorf("wrap file descriptor")
	}
	defer file.Close()

	var before unix.Stat_t
	if err := unix.Fstat(fd, &before); err != nil {
		return nil, err
	}
	if before.Mode&unix.S_IFMT != unix.S_IFREG {
		return nil, fmt.Errorf("%s is not a regular file", relative)
	}
	if before.Size < 0 || before.Size > limit {
		return nil, fmt.Errorf("%s exceeds %d bytes", relative, limit)
	}
	data, err := io.ReadAll(io.LimitReader(file, limit+1))
	if err != nil {
		return nil, err
	}
	if int64(len(data)) > limit {
		return nil, fmt.Errorf("%s exceeds %d bytes", relative, limit)
	}
	var after unix.Stat_t
	if err := unix.Fstat(fd, &after); err != nil {
		return nil, err
	}
	if before.Dev != after.Dev || before.Ino != after.Ino || before.Mode != after.Mode ||
		before.Size != after.Size || before.Mtim != after.Mtim || before.Ctim != after.Ctim ||
		int64(len(data)) != before.Size {
		return nil, fmt.Errorf("%s changed while reading", relative)
	}
	return data, nil
}

func openRegularBeneath(rootFD int, relative string) (int, error) {
	if !pathWithinRoot(filepath.FromSlash(relative)) {
		return -1, fmt.Errorf("path %q escapes the run directory", relative)
	}
	how := &unix.OpenHow{
		Flags: uint64(unix.O_RDONLY | unix.O_CLOEXEC | unix.O_NOFOLLOW),
		Resolve: uint64(unix.RESOLVE_BENEATH | unix.RESOLVE_NO_MAGICLINKS |
			unix.RESOLVE_NO_SYMLINKS),
	}
	fd, err := unix.Openat2(rootFD, relative, how)
	if err == nil {
		return fd, nil
	}
	if errors.Is(err, unix.ELOOP) {
		return -1, fmt.Errorf("path %q traverses symlink: %w", relative, err)
	}
	if !errors.Is(err, unix.ENOSYS) && !errors.Is(err, unix.EINVAL) && !errors.Is(err, unix.EPERM) {
		return -1, err
	}
	return openatNoSymlinks(rootFD, relative)
}

// openatNoSymlinks is the fail-closed fallback for kernels or sandboxes which
// do not expose openat2. Every component is opened relative to an already-open
// directory descriptor with O_NOFOLLOW, so path replacement cannot redirect
// the final read outside the anchored run directory.
func openatNoSymlinks(rootFD int, relative string) (int, error) {
	parts := strings.Split(filepath.ToSlash(relative), "/")
	current, err := unix.Dup(rootFD)
	if err != nil {
		return -1, err
	}
	for index, part := range parts {
		if part == "" || part == "." || part == ".." {
			unix.Close(current)
			return -1, fmt.Errorf("path %q has an unsafe component", relative)
		}
		flags := unix.O_RDONLY | unix.O_CLOEXEC | unix.O_NOFOLLOW
		if index < len(parts)-1 {
			flags |= unix.O_DIRECTORY
		}
		next, openErr := unix.Openat(current, part, flags, 0)
		unix.Close(current)
		if openErr != nil {
			if errors.Is(openErr, unix.ELOOP) {
				return -1, fmt.Errorf("path %q traverses symlink: %w", relative, openErr)
			}
			return -1, openErr
		}
		current = next
	}
	return current, nil
}

func strictDecode(data []byte, target any) error {
	if err := rejectDuplicateJSONKeys(data); err != nil {
		return err
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(target); err != nil {
		return err
	}
	if err := decoder.Decode(&struct{}{}); !errors.Is(err, io.EOF) {
		if err == nil {
			return fmt.Errorf("contains trailing JSON value")
		}
		return fmt.Errorf("trailing data: %w", err)
	}
	return nil
}

func rejectDuplicateJSONKeys(data []byte) error {
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.UseNumber()
	if err := walkJSONValue(decoder); err != nil {
		return err
	}
	if _, err := decoder.Token(); !errors.Is(err, io.EOF) {
		if err == nil {
			return fmt.Errorf("contains trailing JSON value")
		}
		return fmt.Errorf("trailing data: %w", err)
	}
	return nil
}

func walkJSONValue(decoder *json.Decoder) error {
	token, err := decoder.Token()
	if err != nil {
		return err
	}
	delimiter, ok := token.(json.Delim)
	if !ok {
		return nil
	}
	switch delimiter {
	case '{':
		seen := map[string]bool{}
		for decoder.More() {
			keyToken, err := decoder.Token()
			if err != nil {
				return err
			}
			key, ok := keyToken.(string)
			if !ok {
				return fmt.Errorf("object key is not a string")
			}
			if seen[key] {
				return fmt.Errorf("duplicate object key %q", key)
			}
			seen[key] = true
			if err := walkJSONValue(decoder); err != nil {
				return err
			}
		}
		end, err := decoder.Token()
		if err != nil {
			return err
		}
		if end != json.Delim('}') {
			return fmt.Errorf("object has invalid closing delimiter")
		}
	case '[':
		for decoder.More() {
			if err := walkJSONValue(decoder); err != nil {
				return err
			}
		}
		end, err := decoder.Token()
		if err != nil {
			return err
		}
		if end != json.Delim(']') {
			return fmt.Errorf("array has invalid closing delimiter")
		}
	default:
		return fmt.Errorf("unexpected JSON delimiter %q", delimiter)
	}
	return nil
}

func appendPrefixed(dst []string, prefix string, values []string) []string {
	for _, value := range values {
		dst = append(dst, prefix+value)
	}
	return dst
}
