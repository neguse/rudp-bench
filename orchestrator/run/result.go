package run

import (
	"github.com/neguse/rudp-bench/orchestrator/control"
	"github.com/neguse/rudp-bench/orchestrator/netops"
	"github.com/neguse/rudp-bench/orchestrator/sampler"
)

const (
	VerdictValid   = "VALID"
	VerdictInvalid = "INVALID"
)

type Result struct {
	Version        int               `json:"version"`
	Transport      string            `json:"transport"`
	Verdict        string            `json:"verdict"`
	InvalidReasons []string          `json:"invalid_reasons,omitempty"`
	Config         RunConfig         `json:"config"`
	Control        *control.Result   `json:"control,omitempty"`
	Processes      []ProcessResult   `json:"processes"`
	Metrics        *MergedMetrics    `json:"metrics,omitempty"`
	Samples        []sampler.Series  `json:"samples,omitempty"`
	Netem          *NetemResult      `json:"netem,omitempty"`
	Artifacts      map[string]string `json:"artifacts,omitempty"`
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

	Gate *netops.NetemGateReport `json:"gate,omitempty"`
}
