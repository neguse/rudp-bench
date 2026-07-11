package run

import (
	"encoding/json"

	"github.com/neguse/rudp-bench/orchestrator/control"
	"github.com/neguse/rudp-bench/orchestrator/netops"
	"github.com/neguse/rudp-bench/orchestrator/sampler"
)

const (
	VerdictValid   = "VALID"
	VerdictInvalid = "INVALID"
)

type Outcome string

const (
	OutcomePass         Outcome = "PASS"
	OutcomeFail         Outcome = "FAIL"
	OutcomeInvalid      Outcome = "INVALID"
	OutcomeCensored     Outcome = "CENSORED"
	OutcomeUnsupported  Outcome = "UNSUPPORTED"
	OutcomeInconclusive Outcome = "INCONCLUSIVE"
)

type Result struct {
	Version            int                 `json:"version"`
	Transport          string              `json:"transport"`
	Outcome            Outcome             `json:"outcome"`
	OutcomeReasons     []string            `json:"outcome_reasons,omitempty"`
	Verdict            string              `json:"verdict"`
	InvalidReasons     []string            `json:"invalid_reasons,omitempty"`
	Config             RunConfig           `json:"config"`
	Control            *control.Result     `json:"control,omitempty"`
	Processes          []ProcessResult     `json:"processes"`
	Metrics            *MergedMetrics      `json:"metrics,omitempty"`
	ScenarioEvaluation *ScenarioEvaluation `json:"scenario_evaluation,omitempty"`
	Treatment          *TreatmentRecord    `json:"treatment,omitempty"`
	Samples            []sampler.Series    `json:"samples,omitempty"`
	Netem              *NetemResult        `json:"netem,omitempty"`
	Artifacts          map[string]string   `json:"artifacts,omitempty"`
}

type TreatmentRecord struct {
	OrchestratorSHA256 string             `json:"orchestrator_sha256,omitempty"`
	EnvironmentSHA256  string             `json:"environment_sha256"`
	Environment        map[string]string  `json:"environment,omitempty"`
	Host               HostEnvironment    `json:"host"`
	Server             CommandDescription `json:"server"`
	Client             CommandDescription `json:"client"`
}

type CommandDescription struct {
	SHA256       string          `json:"sha256,omitempty"`
	ResolvedPath string          `json:"resolved_path,omitempty"`
	Description  json.RawMessage `json:"description,omitempty"`
	Error        string          `json:"error,omitempty"`
}

type ProcessResult struct {
	Role          string   `json:"role"`
	ProcIndex     int      `json:"proc_index"`
	PID           int      `json:"pid"`
	Conns         int      `json:"conns,omitempty"`
	OriginIDStart int      `json:"origin_id_start,omitempty"`
	OriginIDEnd   int      `json:"origin_id_end,omitempty"`
	Command       []string `json:"command"`
	MetricsOut    string   `json:"metrics_out,omitempty"`
	StdoutPath    string   `json:"stdout_path,omitempty"`
	StderrPath    string   `json:"stderr_path,omitempty"`
	Exited        bool     `json:"exited"`
	ExitCode      int      `json:"exit_code"`
	Error         string   `json:"error,omitempty"`
}

type NetemResult struct {
	Enabled          bool             `json:"enabled"`
	Pair             netops.PairSpec  `json:"pair"`
	SetupCommands    []netops.Command `json:"setup_commands,omitempty"`
	TeardownCommands []netops.Command `json:"teardown_commands,omitempty"`
	UDPBefore        netops.UDPStats  `json:"udp_before,omitempty"`
	UDPAfter         netops.UDPStats  `json:"udp_after,omitempty"`
	UDPDelta         netops.UDPStats  `json:"udp_delta,omitempty"`
	ServerUDPBefore  netops.UDPStats  `json:"server_udp_before,omitempty"`
	ServerUDPAfter   netops.UDPStats  `json:"server_udp_after,omitempty"`
	ServerUDPDelta   netops.UDPStats  `json:"server_udp_delta,omitempty"`

	Gate         *netops.NetemGateReport `json:"gate,omitempty"`
	LossEvidence *NetemLossEvidence      `json:"loss_evidence,omitempty"`
}

// NetemLossEvidence records qdisc counters sampled strictly inside the
// effective control schedule. A positive counter delta therefore cannot be
// attributed solely to setup probes, warmup, or drain traffic.
type NetemLossEvidence struct {
	Version   int                       `json:"version"`
	Mode      string                    `json:"mode"`
	Supported bool                      `json:"supported"`
	Scope     string                    `json:"scope"`
	Schedule  control.ScheduleMessage   `json:"schedule"`
	Before    *netops.QdiscPairSnapshot `json:"before,omitempty"`
	After     *netops.QdiscPairSnapshot `json:"after,omitempty"`
	Delta     *netops.QdiscPairDelta    `json:"delta,omitempty"`
	Errors    []string                  `json:"errors,omitempty"`
}
