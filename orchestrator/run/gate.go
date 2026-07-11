package run

import (
	"encoding/json"
	"fmt"
	"math"

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
	ServerUDPDropDelta netops.UDPStats
	ExtraReasons       []string
	SUTFailureReasons  []string
	Config             *RunConfig
}

type GateResult struct {
	Verdict           string   `json:"verdict"`
	Reasons           []string `json:"reasons,omitempty"`
	AttemptedRatio    float64  `json:"attempted_ratio"`
	SUTFailureReasons []string `json:"sut_failure_reasons,omitempty"`
}

func EvaluateGate(input GateInput) GateResult {
	reasons := append([]string(nil), input.ExtraReasons...)
	sutReasons := append([]string(nil), input.SUTFailureReasons...)

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
	if input.Config != nil && input.Config.Scenario != nil && input.Control != nil {
		invalid, failed := validateScenarioDoneStats(input.Control, *input.Config)
		reasons = append(reasons, invalid...)
		sutReasons = append(sutReasons, failed...)
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
	if input.NetemEnabled && (input.ServerUDPDropDelta.InErrors != 0 || input.ServerUDPDropDelta.RcvbufErrors != 0) {
		sutReasons = append(sutReasons, fmt.Sprintf("server netns UDP drop delta non-zero: InErrors=%d RcvbufErrors=%d", input.ServerUDPDropDelta.InErrors, input.ServerUDPDropDelta.RcvbufErrors))
	}

	verdict := VerdictValid
	if len(reasons) > 0 {
		verdict = VerdictInvalid
	}
	return GateResult{
		Verdict:           verdict,
		Reasons:           dedupeStrings(reasons),
		AttemptedRatio:    attempted,
		SUTFailureReasons: dedupeStrings(sutReasons),
	}
}

type authoritativeProgress struct {
	Role                        string `json:"role"`
	LocalConns                  uint64 `json:"local_conns"`
	RosterConns                 uint64 `json:"roster_conns"`
	InputLastSentMin            uint64 `json:"input_last_sent_min"`
	InputLastSentMax            uint64 `json:"input_last_sent_max"`
	StateHeaderSeqRecvMin       uint64 `json:"state_header_seq_recv_min"`
	StateHeaderSeqRecvMax       uint64 `json:"state_header_seq_recv_max"`
	StateAppliedInputSeqRecvMin uint64 `json:"state_applied_input_seq_recv_min"`
	StateAppliedInputSeqRecvMax uint64 `json:"state_applied_input_seq_recv_max"`
	ServerStateTicks            uint64 `json:"server_state_ticks"`
}

type scenarioDoneStats struct {
	InvalidPayload        *uint64                `json:"invalid_payload"`
	AuthoritativeProgress *authoritativeProgress `json:"authoritative_progress"`
}

func validateScenarioDoneStats(controlResult *control.Result, cfg RunConfig) (invalid, failed []string) {
	serverCount := 0
	var losslessFinalStateSeq uint64
	losslessFinalStateSet := false
	for _, participant := range controlResult.Participants {
		context := fmt.Sprintf("%s proc_index=%d done.stats", participant.Hello.Role, participant.Hello.ProcIndex)
		var stats scenarioDoneStats
		if len(participant.Done.Stats) == 0 || json.Unmarshal(participant.Done.Stats, &stats) != nil {
			invalid = append(invalid, context+" is not a JSON object")
			continue
		}
		if stats.InvalidPayload == nil {
			invalid = append(invalid, context+" missing invalid_payload")
		} else if *stats.InvalidPayload != 0 {
			failed = append(failed, fmt.Sprintf("%s payload corruption count=%d", context, *stats.InvalidPayload))
		}
		if cfg.Scenario.Kind != ScenarioAuthoritativeState {
			continue
		}
		progress := stats.AuthoritativeProgress
		if progress == nil {
			invalid = append(invalid, context+" missing authoritative_progress")
			continue
		}
		if progress.Role != participant.Hello.Role {
			invalid = append(invalid, fmt.Sprintf("%s role=%q, want %q", context, progress.Role, participant.Hello.Role))
		}
		switch participant.Hello.Role {
		case "server":
			serverCount++
			if progress.RosterConns != uint64(cfg.TotalConns) {
				failed = append(failed, fmt.Sprintf("%s roster_conns=%d, want %d", context, progress.RosterConns, cfg.TotalConns))
			}
			minTicks, maxTicks, err := expectedSlotRange(cfg.Scenario.ServerState.LossTolerant.RateHz, cfg.Duration.Duration, 1)
			if err != nil || progress.ServerStateTicks < minTicks || progress.ServerStateTicks > maxTicks {
				failed = append(failed, fmt.Sprintf("%s server_state_ticks=%d, want %d..%d independent global ticks", context, progress.ServerStateTicks, minTicks, maxTicks))
			}
		case "client":
			if progress.LocalConns != uint64(participant.Ready.Conns) || progress.RosterConns != uint64(cfg.TotalConns) {
				failed = append(failed, fmt.Sprintf("%s connection coverage local=%d/%d roster=%d/%d", context,
					progress.LocalConns, participant.Ready.Conns, progress.RosterConns, cfg.TotalConns))
			}
			for name, value := range map[string]uint64{
				"input_last_sent_min":              progress.InputLastSentMin,
				"state_header_seq_recv_min":        progress.StateHeaderSeqRecvMin,
				"state_applied_input_seq_recv_min": progress.StateAppliedInputSeqRecvMin,
			} {
				if value == 0 {
					failed = append(failed, fmt.Sprintf("%s %s=0; not every local connection demonstrated progress", context, name))
				}
			}
			if progress.InputLastSentMin > progress.InputLastSentMax ||
				progress.StateHeaderSeqRecvMin > progress.StateHeaderSeqRecvMax ||
				progress.StateAppliedInputSeqRecvMin > progress.StateAppliedInputSeqRecvMax {
				invalid = append(invalid, context+" has inverted authoritative progress range")
			}
			if progress.StateAppliedInputSeqRecvMax > progress.InputLastSentMax {
				failed = append(failed, fmt.Sprintf("%s applied input seq=%d exceeds locally sent seq=%d", context,
					progress.StateAppliedInputSeqRecvMax, progress.InputLastSentMax))
			}
			if losslessScenario(cfg.Netem) && progress.StateAppliedInputSeqRecvMin+1 < progress.InputLastSentMin {
				failed = append(failed, fmt.Sprintf("%s final applied input min=%d did not catch up to sent min=%d", context,
					progress.StateAppliedInputSeqRecvMin, progress.InputLastSentMin))
			}
			if losslessScenario(cfg.Netem) && progress.StateHeaderSeqRecvMin != progress.StateHeaderSeqRecvMax {
				failed = append(failed, fmt.Sprintf("%s final global state tick differs across local connections: min=%d max=%d", context,
					progress.StateHeaderSeqRecvMin, progress.StateHeaderSeqRecvMax))
			}
			if losslessScenario(cfg.Netem) {
				if losslessFinalStateSet && progress.StateHeaderSeqRecvMax != losslessFinalStateSeq {
					failed = append(failed, fmt.Sprintf("%s final global state tick=%d differs from another client process=%d", context,
						progress.StateHeaderSeqRecvMax, losslessFinalStateSeq))
				}
				losslessFinalStateSeq = progress.StateHeaderSeqRecvMax
				losslessFinalStateSet = true
			}
		default:
			invalid = append(invalid, context+" has unknown role")
		}
	}
	if cfg.Scenario.Kind == ScenarioAuthoritativeState && serverCount != 1 {
		invalid = append(invalid, fmt.Sprintf("authoritative progress has %d server records, want 1", serverCount))
	}
	return dedupeStrings(invalid), dedupeStrings(failed)
}

func losslessScenario(netem *NetemRegime) bool {
	return netem == nil || (math.Abs(netem.ClientEgress.LossPercent) < 1e-12 && math.Abs(netem.ServerEgress.LossPercent) < 1e-12)
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
