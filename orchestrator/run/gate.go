package run

import (
	"fmt"

	"github.com/neguse/rudp-bench/orchestrator/control"
	"github.com/neguse/rudp-bench/orchestrator/netops"
)

type GateInput struct {
	Control            *control.Result
	Metrics            *MergedMetrics
	Processes          []ProcessResult
	AttemptedThreshold float64
	NetemEnabled       bool
	UDPDropDelta       netops.UDPStats
	ExtraReasons       []string
}

type GateResult struct {
	Verdict        string   `json:"verdict"`
	Reasons        []string `json:"reasons,omitempty"`
	AttemptedRatio float64  `json:"attempted_ratio"`
}

func EvaluateGate(input GateInput) GateResult {
	reasons := append([]string(nil), input.ExtraReasons...)

	if input.Control == nil {
		reasons = append(reasons, "control result is missing")
	} else {
		reasons = append(reasons, input.Control.InvalidReasons...)
		for _, p := range input.Control.Participants {
			if p.AckReceived && p.SchedAck.MarginNS < 0 {
				reasons = append(reasons, fmt.Sprintf("proc_index=%d pid=%d negative schedule margin: %d ns", p.Hello.ProcIndex, p.Hello.PID, p.SchedAck.MarginNS))
			}
		}
	}

	attempted := 0.0
	if input.Metrics == nil {
		reasons = append(reasons, "merged metrics are missing")
	} else {
		attempted = input.Metrics.attemptedRatio()
		if attempted < input.AttemptedThreshold {
			reasons = append(reasons, fmt.Sprintf("attempted_ratio=%g below threshold=%g", attempted, input.AttemptedThreshold))
		}
	}

	for _, p := range input.Processes {
		if !p.Exited {
			reasons = append(reasons, fmt.Sprintf("%s proc_index=%d pid=%d did not exit", p.Role, p.ProcIndex, p.PID))
			continue
		}
		if p.ExitCode != 0 {
			if p.Error != "" {
				reasons = append(reasons, fmt.Sprintf("%s proc_index=%d pid=%d exit_code=%d: %s", p.Role, p.ProcIndex, p.PID, p.ExitCode, p.Error))
			} else {
				reasons = append(reasons, fmt.Sprintf("%s proc_index=%d pid=%d exit_code=%d", p.Role, p.ProcIndex, p.PID, p.ExitCode))
			}
		}
	}

	if input.NetemEnabled && (input.UDPDropDelta.InErrors != 0 || input.UDPDropDelta.RcvbufErrors != 0) {
		reasons = append(reasons, fmt.Sprintf("client netns UDP drop delta non-zero: InErrors=%d RcvbufErrors=%d", input.UDPDropDelta.InErrors, input.UDPDropDelta.RcvbufErrors))
	}

	verdict := VerdictValid
	if len(reasons) > 0 {
		verdict = VerdictInvalid
	}
	return GateResult{
		Verdict:        verdict,
		Reasons:        dedupeStrings(reasons),
		AttemptedRatio: attempted,
	}
}

func dedupeStrings(in []string) []string {
	seen := make(map[string]bool, len(in))
	out := make([]string, 0, len(in))
	for _, s := range in {
		if s == "" || seen[s] {
			continue
		}
		seen[s] = true
		out = append(out, s)
	}
	return out
}
