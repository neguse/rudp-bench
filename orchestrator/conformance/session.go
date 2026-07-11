package conformance

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/netops"
	"github.com/neguse/rudp-bench/orchestrator/run"
)

const (
	SessionConfigVersion = 1
	SessionPlanVersion   = 1
	sessionIdentityV1    = "class-mapping-acquisition-session-v1"
	slotIdentityV1       = "class-mapping-acquisition-slot-v1"
)

// TransportSpec contains only endpoint launch information which may vary by
// transport. The conformance topology itself is fixed by Probe.
type TransportSpec struct {
	ServerCommand    run.CommandConfig `json:"server_command"`
	ClientCommand    run.CommandConfig `json:"client_command"`
	ClientProcs      int               `json:"client_procs"`
	SchedIsMeasurand bool              `json:"sched_is_measurand,omitempty"`
	// Diagnostic transports establish a measurement floor. Their explicitly
	// unsupported cases do not disqualify candidate solution reports.
	Diagnostic bool `json:"diagnostic,omitempty"`
}

// SessionConfig is the user-authored acquisition contract. OutputDir and
// DoctorReport are artifact locators; their paths are not measurement
// treatment. Doctor contents, when supplied, are bound separately.
type SessionConfig struct {
	Version      int                      `json:"version"`
	Probe        Config                   `json:"probe"`
	DoctorReport string                   `json:"doctor_report,omitempty"`
	Transports   map[string]TransportSpec `json:"transports"`
	ServerCPUs   string                   `json:"server_cpus,omitempty"`
	ClientCPUs   string                   `json:"client_cpus,omitempty"`
	OutputDir    string                   `json:"output_dir"`
}

// Preflight binds endpoint binaries to their mutually consistent --describe
// class mappings before any acquisition is planned.
type Preflight struct {
	Transport     string                 `json:"transport"`
	ServerSHA256  string                 `json:"server_sha256"`
	ClientSHA256  string                 `json:"client_sha256"`
	Mapping       run.ClassMappingRecord `json:"mapping"`
	MappingSHA256 string                 `json:"mapping_sha256"`
	ProbeIdentity string                 `json:"probe_identity"`
}

type AttemptPlan struct {
	Transport     string        `json:"transport"`
	CaseID        ProbeCaseID   `json:"case_id"`
	AttemptNumber int           `json:"attempt_number"`
	SlotIdentity  string        `json:"slot_identity"`
	RunIdentity   string        `json:"run_identity"`
	AcquisitionID string        `json:"acquisition_id"`
	RunConfig     run.RunConfig `json:"run_config"`
}

type CasePlan struct {
	Transport string `json:"transport"`
	CaseDefinition
	MappingSHA256         string                `json:"mapping_sha256"`
	EndpointMappingSHA256 string                `json:"endpoint_mapping_sha256"`
	ProbeIdentity         string                `json:"probe_identity"`
	CaseIdentity          string                `json:"case_identity"`
	DeclaredMapping       *run.ClassMappingSpec `json:"declared_mapping,omitempty"`
	RequiredAcquisitions  int                   `json:"required_acquisitions"`
	Unsupported           bool                  `json:"unsupported"`
	SkipReasons           []string              `json:"skip_reasons,omitempty"`
	Attempts              []AttemptPlan         `json:"attempts,omitempty"`
}

type SessionPlan struct {
	Version            int           `json:"version"`
	SessionIdentity    string        `json:"session_identity"`
	ConfigSHA256       string        `json:"config_sha256"`
	DoctorSHA256       string        `json:"doctor_sha256,omitempty"`
	OrchestratorSHA256 string        `json:"orchestrator_sha256"`
	EnvironmentSHA256  string        `json:"environment_sha256"`
	Config             SessionConfig `json:"config"`
	Preflights         []Preflight   `json:"preflights"`
	Cases              []CasePlan    `json:"cases"`
}

func LoadSessionConfig(path string) (SessionConfig, error) {
	var config SessionConfig
	data, err := os.ReadFile(path)
	if err != nil {
		return config, err
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&config); err != nil {
		return config, err
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		return config, fmt.Errorf("session config must contain exactly one JSON object")
	}
	return config.Prepare()
}

// Prepare validates and deep-copies a programmatically supplied session
// config. In particular, later RunConfig.Prepare calls cannot append scenario
// arguments into caller-owned command slices.
func (config SessionConfig) Prepare() (SessionConfig, error) {
	prepared := cloneSessionConfig(config)
	if prepared.Version != SessionConfigVersion {
		return SessionConfig{}, fmt.Errorf("session config version=%d, want %d", prepared.Version, SessionConfigVersion)
	}
	if reasons := prepared.Probe.ValidateCanonical(); len(reasons) != 0 {
		return SessionConfig{}, fmt.Errorf("probe: %s", strings.Join(reasons, "; "))
	}
	if strings.TrimSpace(prepared.OutputDir) == "" {
		return SessionConfig{}, fmt.Errorf("output_dir is required")
	}
	if strings.IndexByte(prepared.OutputDir, 0) >= 0 {
		return SessionConfig{}, fmt.Errorf("output_dir contains a NUL byte")
	}
	if strings.IndexByte(prepared.DoctorReport, 0) >= 0 {
		return SessionConfig{}, fmt.Errorf("doctor_report contains a NUL byte")
	}
	prepared.OutputDir = filepath.Clean(prepared.OutputDir)
	if prepared.DoctorReport != "" {
		prepared.DoctorReport = filepath.Clean(prepared.DoctorReport)
	}
	if len(prepared.Transports) == 0 {
		return SessionConfig{}, fmt.Errorf("at least one transport is required")
	}
	var reasons []string
	promotionCandidates := 0
	for name, transport := range prepared.Transports {
		if !run.IsSafeName(name) {
			reasons = append(reasons, fmt.Sprintf("transport %q must be a path-safe ASCII slug", name))
		}
		if transport.ClientProcs != 1 {
			reasons = append(reasons, fmt.Sprintf("transport %q client_procs=%d, want exactly 1", name, transport.ClientProcs))
		}
		if !transport.Diagnostic {
			promotionCandidates++
		}
		for role, command := range map[string]run.CommandConfig{
			"client": transport.ClientCommand,
			"server": transport.ServerCommand,
		} {
			if strings.TrimSpace(command.Path) == "" {
				reasons = append(reasons, fmt.Sprintf("transport %q %s_command.path is required", name, role))
			}
			if commandContainsNUL(command) {
				reasons = append(reasons, fmt.Sprintf("transport %q %s_command contains a NUL byte", name, role))
			}
		}
	}
	if promotionCandidates == 0 {
		reasons = append(reasons, "at least one non-diagnostic transport is required")
	}
	sort.Strings(reasons)
	if len(reasons) != 0 {
		return SessionConfig{}, fmt.Errorf("%s", strings.Join(reasons, "; "))
	}
	return prepared, nil
}

// BuildSessionPlan performs endpoint --describe preflight and produces a
// deterministic acquisition plan. It does not create directories or execute
// a benchmark run.
func BuildSessionPlan(ctx context.Context, config SessionConfig) (SessionPlan, error) {
	var plan SessionPlan
	if ctx == nil {
		return plan, fmt.Errorf("context is required")
	}
	prepared, err := config.Prepare()
	if err != nil {
		return plan, err
	}
	doctorSHA, err := doctorFingerprint(prepared.DoctorReport)
	if err != nil {
		return plan, err
	}

	transportNames := sortedTransportNames(prepared.Transports)
	preflights := make([]Preflight, 0, len(transportNames))
	var preflightReasons []string
	for _, name := range transportNames {
		transport := prepared.Transports[name]
		serverSHA := run.CommandFingerprint(transport.ServerCommand)
		clientSHA := run.CommandFingerprint(transport.ClientCommand)
		if !isUsableCommandFingerprint(serverSHA) {
			preflightReasons = append(preflightReasons, fmt.Sprintf("transport %q server command fingerprint is %q", name, serverSHA))
		}
		if !isUsableCommandFingerprint(clientSHA) {
			preflightReasons = append(preflightReasons, fmt.Sprintf("transport %q client command fingerprint is %q", name, clientSHA))
		}

		mapping, mappingErr := run.PreflightClassMapping(ctx, name, transport.ServerCommand, transport.ClientCommand)
		if mappingErr != nil {
			preflightReasons = append(preflightReasons, fmt.Sprintf("transport %q mapping preflight: %v", name, mappingErr))
			continue
		}
		serverAfter := run.CommandFingerprint(transport.ServerCommand)
		clientAfter := run.CommandFingerprint(transport.ClientCommand)
		if serverAfter != serverSHA || clientAfter != clientSHA {
			preflightReasons = append(preflightReasons, fmt.Sprintf("transport %q endpoint command changed during mapping preflight", name))
			continue
		}
		mapping = cloneMapping(mapping)
		mappingSHA := MappingSHA256(mapping)
		preflights = append(preflights, Preflight{
			Transport: name, ServerSHA256: serverSHA, ClientSHA256: clientSHA,
			Mapping: mapping, MappingSHA256: mappingSHA,
			ProbeIdentity: ProbeIdentity(prepared.Probe, name, mappingSHA),
		})
	}
	if len(preflightReasons) != 0 {
		sort.Strings(preflightReasons)
		return plan, fmt.Errorf("preflight failed: %s", strings.Join(preflightReasons, "; "))
	}

	orchestratorSHA := run.OrchestratorFingerprint()
	environmentSHA := run.EnvironmentFingerprint()
	if !isSHA256Digest(orchestratorSHA) || !isSHA256Digest(environmentSHA) {
		return plan, fmt.Errorf("orchestrator/environment fingerprints are unavailable")
	}
	identityConfig := cloneSessionConfig(prepared)
	identityConfig.OutputDir = ""
	identityConfig.DoctorReport = ""
	configSHA := run.HashValue(identityConfig)
	sessionIdentity := run.HashValue(struct {
		Contract           string      `json:"contract"`
		ConfigSHA256       string      `json:"config_sha256"`
		DoctorSHA256       string      `json:"doctor_sha256,omitempty"`
		OrchestratorSHA256 string      `json:"orchestrator_sha256"`
		EnvironmentSHA256  string      `json:"environment_sha256"`
		Preflights         []Preflight `json:"preflights"`
	}{sessionIdentityV1, configSHA, doctorSHA, orchestratorSHA, environmentSHA, preflights})

	plan = SessionPlan{
		Version: SessionPlanVersion, SessionIdentity: sessionIdentity,
		ConfigSHA256: configSHA, DoctorSHA256: doctorSHA,
		OrchestratorSHA256: orchestratorSHA, EnvironmentSHA256: environmentSHA,
		Config: cloneSessionConfig(prepared), Preflights: clonePreflights(preflights),
	}
	definitions := RequiredCases()
	sort.Slice(definitions, func(i, j int) bool { return definitions[i].ID < definitions[j].ID })
	usedPrefixes := make(map[string]bool)
	for _, preflight := range preflights {
		transport := prepared.Transports[preflight.Transport]
		for _, definition := range definitions {
			casePlan, err := planCase(prepared, sessionIdentity, preflight, transport, definition, usedPrefixes)
			if err != nil {
				return SessionPlan{}, err
			}
			plan.Cases = append(plan.Cases, casePlan)
		}
	}
	return plan, nil
}

func planCase(config SessionConfig, sessionIdentity string, preflight Preflight, transport TransportSpec,
	definition CaseDefinition, usedPrefixes map[string]bool,
) (CasePlan, error) {
	casePlan := CasePlan{
		Transport: preflight.Transport, CaseDefinition: definition,
		MappingSHA256:         preflight.MappingSHA256,
		EndpointMappingSHA256: endpointMappingSHA256(preflight.Mapping, definition, preflight.MappingSHA256),
		ProbeIdentity:         preflight.ProbeIdentity,
		CaseIdentity:          CaseIdentity(preflight.ProbeIdentity, definition),
		RequiredAcquisitions:  config.Probe.ValidAcquisitionsPerCase,
	}
	declared, supported, reasons := mappingForCase(preflight.Mapping, definition)
	if len(reasons) != 0 {
		return CasePlan{}, fmt.Errorf("transport %q case %q mapping: %s", preflight.Transport, definition.ID, strings.Join(reasons, "; "))
	}
	if declared != nil {
		copy := *declared
		casePlan.DeclaredMapping = &copy
	}
	if !supported {
		casePlan.Unsupported = true
		casePlan.SkipReasons = []string{"declared mapping is unsupported"}
		return casePlan, nil
	}

	for attemptNumber := 1; attemptNumber <= config.Probe.MaxAttemptsPerCase; attemptNumber++ {
		prefixSeed := run.HashValue(struct {
			SessionIdentity string      `json:"session_identity"`
			Transport       string      `json:"transport"`
			CaseID          ProbeCaseID `json:"case_id"`
			AttemptNumber   int         `json:"attempt_number"`
		}{sessionIdentity, preflight.Transport, definition.ID, attemptNumber})
		prefix := "cm" + prefixSeed[:9]
		if usedPrefixes[prefix] {
			return CasePlan{}, fmt.Errorf("internal prefix collision for transport %q case %q attempt %d", preflight.Transport, definition.ID, attemptNumber)
		}
		usedPrefixes[prefix] = true
		runConfig, err := probeRunConfig(config, preflight, transport, definition, attemptNumber, prefix)
		if err != nil {
			return CasePlan{}, fmt.Errorf("transport %q case %q attempt %d: %w", preflight.Transport, definition.ID, attemptNumber, err)
		}
		runIdentity := run.ConfigIdentity(runConfig)
		slotIdentity := run.HashValue(struct {
			Contract        string      `json:"contract"`
			SessionIdentity string      `json:"session_identity"`
			CaseIdentity    string      `json:"case_identity"`
			CaseID          ProbeCaseID `json:"case_id"`
			AttemptNumber   int         `json:"attempt_number"`
			RunIdentity     string      `json:"run_identity"`
		}{slotIdentityV1, sessionIdentity, casePlan.CaseIdentity, definition.ID, attemptNumber, runIdentity})
		acquisitionID := AcquisitionIdentity(casePlan.CaseIdentity, attemptNumber, runIdentity)
		casePlan.Attempts = append(casePlan.Attempts, AttemptPlan{
			Transport: preflight.Transport, CaseID: definition.ID, AttemptNumber: attemptNumber,
			SlotIdentity: slotIdentity, RunIdentity: runIdentity, AcquisitionID: acquisitionID,
			RunConfig: runConfig,
		})
	}
	return casePlan, nil
}

func probeRunConfig(config SessionConfig, preflight Preflight, transport TransportSpec,
	definition CaseDefinition, attemptNumber int, prefix string,
) (run.RunConfig, error) {
	selected := run.TrafficClassSpec{
		RateHz: float64(config.Probe.RateHz), PayloadBytes: int(config.Probe.PayloadBytes),
	}
	traffic := run.TrafficSpec{TrafficID: run.TrafficIDClientInput}
	switch definition.Class {
	case run.ClassLossTolerant:
		traffic.LossTolerant = selected
	case run.ClassMustDeliver:
		traffic.MustDeliver = selected
	default:
		return run.RunConfig{}, fmt.Errorf("unknown traffic class %q", definition.Class)
	}
	scenario := run.ScenarioSpec{
		Name:        "class-mapping-" + strings.ReplaceAll(definition.Class, "_", "-"),
		Kind:        run.ScenarioEnvironmentBaseline,
		ClientInput: &traffic,
	}
	netem := &run.NetemRegime{
		Prefix: prefix, LinkMTUBytes: int(config.Probe.LinkMTUBytes), DisableOffloads: true,
	}
	switch definition.Egress {
	case EgressNone:
	case EgressClient:
		netem.ClientEgress = netops.Netem{LossPercent: config.Probe.LossPercent}
	case EgressServer:
		netem.ServerEgress = netops.Netem{LossPercent: config.Probe.LossPercent}
	default:
		return run.RunConfig{}, fmt.Errorf("unknown loss egress %q", definition.Egress)
	}
	output := filepath.Join(config.OutputDir, preflight.Transport, string(definition.ID), fmt.Sprintf("attempt-%02d", attemptNumber))
	runConfig := run.RunConfig{
		Transport: preflight.Transport, ClassMappingSHA256: preflight.Mapping.ServerSHA256,
		Scenario:      &scenario,
		ServerCommand: cloneCommandConfig(transport.ServerCommand),
		ClientCommand: cloneCommandConfig(transport.ClientCommand),
		ClientProcs:   1, TotalConns: config.Probe.TotalConnections,
		Warmup:   run.Duration{Duration: time.Duration(config.Probe.WarmupNS)},
		Duration: run.Duration{Duration: time.Duration(config.Probe.DurationNS)},
		Drain:    run.Duration{Duration: time.Duration(config.Probe.DrainNS)},
		Netem:    netem, ServerCPUs: config.ServerCPUs, ClientCPUs: config.ClientCPUs,
		SchedIsMeasurand:   transport.SchedIsMeasurand,
		AttemptedThreshold: 1,
		OutputDir:          output,
	}
	return runConfig.Prepare()
}

func cloneSessionConfig(config SessionConfig) SessionConfig {
	cloned := config
	if config.Transports == nil {
		return cloned
	}
	cloned.Transports = make(map[string]TransportSpec, len(config.Transports))
	for name, transport := range config.Transports {
		transport.ServerCommand = cloneCommandConfig(transport.ServerCommand)
		transport.ClientCommand = cloneCommandConfig(transport.ClientCommand)
		cloned.Transports[name] = transport
	}
	return cloned
}

func cloneCommandConfig(command run.CommandConfig) run.CommandConfig {
	command.Args = append([]string(nil), command.Args...)
	command.Env = append([]string(nil), command.Env...)
	return command
}

func clonePreflights(preflights []Preflight) []Preflight {
	cloned := make([]Preflight, len(preflights))
	copy(cloned, preflights)
	for index := range cloned {
		cloned[index].Mapping = cloneMapping(preflights[index].Mapping)
	}
	return cloned
}

func sortedTransportNames(transports map[string]TransportSpec) []string {
	names := make([]string, 0, len(transports))
	for name := range transports {
		names = append(names, name)
	}
	sort.Strings(names)
	return names
}

func doctorFingerprint(path string) (string, error) {
	if path == "" {
		return "", nil
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return "", fmt.Errorf("doctor_report: %w", err)
	}
	return run.HashBytes(data), nil
}

func commandContainsNUL(command run.CommandConfig) bool {
	if strings.IndexByte(command.Path, 0) >= 0 || strings.IndexByte(command.Dir, 0) >= 0 {
		return true
	}
	for _, values := range [][]string{command.Args, command.Env} {
		for _, value := range values {
			if strings.IndexByte(value, 0) >= 0 {
				return true
			}
		}
	}
	return false
}

func isUsableCommandFingerprint(value string) bool {
	return value != "unresolved" && value != "unreadable" && isSHA256Digest(value)
}

func isSHA256Digest(value string) bool {
	if len(value) != 64 {
		return false
	}
	for _, char := range value {
		if (char < '0' || char > '9') && (char < 'a' || char > 'f') {
			return false
		}
	}
	return true
}
