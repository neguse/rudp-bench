package sweep

import "github.com/neguse/rudp-bench/orchestrator/run"

const comparisonIdentityVersion = 4

// comparisonIdentity identifies one capacity cell's comparable treatment. It
// intentionally excludes replicate-specific values while retaining everything
// that changes the scenario, SLO, resource budget, or measured treatment.
func comparisonIdentity(cfg Config, cell cellDefinition) string {
	transport := cfg.Transports[cell.Transport]
	record := struct {
		Version            int               `json:"version"`
		Regime             string            `json:"regime"`
		Transport          string            `json:"transport"`
		TransportSpec      TransportSpec     `json:"transport_spec"`
		Workload           string            `json:"workload,omitempty"`
		Scenario           *run.ScenarioSpec `json:"scenario,omitempty"`
		Conns              ConnsRange        `json:"conns"`
		Warmup             run.Duration      `json:"warmup"`
		SteadyWarmup       bool              `json:"steady_warmup"`
		Drain              run.Duration      `json:"drain"`
		Duration           run.Duration      `json:"duration"`
		DeadlineNS         uint64            `json:"deadline_ns"`
		StalenessPeriodNS  uint64            `json:"staleness_period_ns"`
		Netem              *run.NetemRegime  `json:"netem,omitempty"`
		ServerCPUs         string            `json:"server_cpus,omitempty"`
		ClientCPUs         string            `json:"client_cpus,omitempty"`
		ServerSHA256       string            `json:"server_sha256,omitempty"`
		ClientSHA256       string            `json:"client_sha256,omitempty"`
		OrchestratorSHA256 string            `json:"orchestrator_sha256,omitempty"`
		EnvironmentSHA256  string            `json:"environment_sha256"`
		MeasurementMode    string            `json:"measurement_mode"`
		DoctorSHA256       string            `json:"doctor_sha256,omitempty"`
	}{
		Version:            comparisonIdentityVersion,
		Regime:             cfg.Regime,
		Transport:          cell.Transport,
		TransportSpec:      transport,
		Workload:           cell.Workload,
		Scenario:           cell.Scenario,
		Conns:              cfg.Conns,
		Warmup:             cfg.Warmup,
		SteadyWarmup:       cfg.SteadyWarmup,
		Drain:              cfg.Drain,
		Duration:           cfg.Duration,
		DeadlineNS:         cfg.DeadlineNS,
		StalenessPeriodNS:  cfg.StalenessPeriodNS,
		Netem:              comparisonNetem(cfg.Netem),
		ServerCPUs:         cfg.ServerCPUs,
		ClientCPUs:         cfg.ClientCPUs,
		ServerSHA256:       run.CommandFingerprint(transport.ServerCommand),
		ClientSHA256:       run.CommandFingerprint(transport.ClientCommand),
		OrchestratorSHA256: run.OrchestratorFingerprint(),
		EnvironmentSHA256:  run.EnvironmentFingerprint(),
		MeasurementMode:    cfg.MeasurementMode,
		DoctorSHA256:       doctorReportFingerprint(cfg.DoctorReport),
	}
	return run.HashValue(record)
}

func comparisonNetem(netem *run.NetemRegime) *run.NetemRegime {
	if netem == nil {
		return nil
	}
	normalized := *netem
	normalized.ServerEgress.LossSeed = 0
	normalized.ClientEgress.LossSeed = 0
	return &normalized
}
