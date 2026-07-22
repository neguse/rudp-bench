package run

import (
	"encoding/json"
	"fmt"
	"math"

	"github.com/neguse/rudp-bench/orchestrator/control"
	"github.com/neguse/rudp-bench/orchestrator/netops"
	"github.com/neguse/rudp-bench/orchestrator/netops/losstrace"
)

type GateInput struct {
	Control            *control.Result
	Metrics            *MergedMetrics
	Processes          []ProcessResult
	AttemptedThreshold float64
	NetemEnabled       bool
	UDPDropDelta       netops.UDPStats
	ServerUDPDropDelta netops.UDPStats
	Offloads           *netops.OffloadEvidence
	OffloadsAfter      *netops.OffloadEvidence
	LossEvidence       *NetemLossEvidence
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
	if input.Config != nil && input.Config.Ramp == nil && input.Config.Scenario != nil && input.Control != nil {
		invalid, failed := validateScenarioDoneStats(input.Control, *input.Config)
		reasons = append(reasons, invalid...)
		sutReasons = append(sutReasons, failed...)
	}

	attempted := 0.0
	if input.Config != nil && input.Config.Ramp != nil {
		// Ramp runs are judged from per-phase suffix snapshots. Their final
		// cumulative metrics intentionally span multiple active-connection
		// levels and therefore do not satisfy the fixed-window denominator.
	} else if input.Metrics == nil {
		reasons = append(reasons, "merged metrics are missing")
	} else {
		attempted = input.Metrics.attemptedRatio()
		if attempted < input.AttemptedThreshold {
			// peer 切断が開示されている run の attempted 不足は、切断された
			// conn の slot が未送計上された結果 = SUT の切断帰属(ADR-0002 の
			// 「切断は FAIL」)。開示なしの不足は従来どおり farm 側疑いの INVALID
			if disconnects := totalPeerDisconnects(input.Control); disconnects > 0 {
				sutReasons = append(sutReasons, fmt.Sprintf(
					"attempted_ratio=%g below threshold=%g with peer_disconnects=%d: unsent slots attributed to SUT disconnects",
					attempted, input.AttemptedThreshold, disconnects))
			} else {
				reasons = append(reasons, fmt.Sprintf("attempted_ratio=%g below threshold=%g", attempted, input.AttemptedThreshold))
			}
		}
		if input.Metrics.Raw.TimestampOrderViolations != 0 {
			reasons = append(reasons, fmt.Sprintf("timestamp_order_violations=%d, want 0",
				input.Metrics.Raw.TimestampOrderViolations))
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
	reasons = append(reasons, ValidateOffloadIntervalEvidence(input.Config, input.Offloads, input.OffloadsAfter, len(sutReasons) > 0)...)
	reasons = append(reasons, ValidateNetemLossEvidence(input.Config, input.Control, input.LossEvidence)...)

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

// ValidateOffloadEvidence is opt-in: legacy runs do not need ethtool or carry
// offload evidence unless disable_offloads is part of their netem treatment.
func ValidateOffloadEvidence(cfg *RunConfig, evidence *netops.OffloadEvidence) []string {
	if cfg == nil || cfg.Netem == nil || !cfg.Netem.DisableOffloads {
		return nil
	}
	var reasons []string
	for _, reason := range netops.ValidateOffloadEvidence(cfg.Netem.pairSpec(), evidence) {
		reasons = append(reasons, "offload evidence: "+reason)
	}
	return reasons
}

// ValidateOffloadIntervalEvidence proves the packet-shaping feature and MTU
// treatment both before and after a completed measurement. A process failure
// may prevent the after sample, but any after sample that exists is still
// validated.
func ValidateOffloadIntervalEvidence(cfg *RunConfig, before, after *netops.OffloadEvidence, allowMissingAfter bool) []string {
	reasons := ValidateOffloadEvidence(cfg, before)
	if cfg == nil || cfg.Netem == nil || !cfg.Netem.DisableOffloads {
		return reasons
	}
	if after == nil {
		if !allowMissingAfter {
			reasons = append(reasons, "offload evidence after measurement: missing")
		}
		return reasons
	}
	for _, reason := range netops.ValidateOffloadEvidence(cfg.Netem.pairSpec(), after) {
		reasons = append(reasons, "offload evidence after measurement: "+reason)
	}
	return reasons
}

// ValidateNetemLossEvidence validates random-netem qdisc counter evidence
// independently of the setup ping/iperf preflight. Counter deltas are
// recomputed from the snapshots and must be wholly contained in the effective
// measurement schedule.
func ValidateNetemLossEvidence(cfg *RunConfig, controlResult *control.Result, evidence *NetemLossEvidence) []string {
	if cfg == nil || !configuredLoss(cfg.Netem) {
		return nil
	}
	prefix := func(reason string) string { return "netem loss evidence: " + reason }
	if deterministicLoss(cfg.Netem) {
		return validateDeterministicLossEvidence(cfg, controlResult, evidence, prefix)
	}
	if evidence == nil {
		return []string{prefix("missing for configured random loss")}
	}
	var reasons []string
	if evidence.Version != 1 {
		reasons = append(reasons, prefix(fmt.Sprintf("version=%d, want 1", evidence.Version)))
	}
	if evidence.Mode != lossEvidenceModeRandomNetem {
		reasons = append(reasons, prefix(fmt.Sprintf("mode=%q, want %q", evidence.Mode, lossEvidenceModeRandomNetem)))
	}
	if !evidence.Supported {
		reasons = append(reasons, prefix("random netem qdisc accounting marked unsupported"))
	}
	if evidence.Scope != lossEvidenceScopeEffectiveInner {
		reasons = append(reasons, prefix(fmt.Sprintf("scope=%q does not prove in-window loss", evidence.Scope)))
	}
	for _, reason := range evidence.Errors {
		reasons = append(reasons, prefix(reason))
	}
	if controlResult == nil {
		reasons = append(reasons, prefix("control schedule is unavailable"))
	} else if evidence.Schedule != controlResult.Schedule {
		reasons = append(reasons, prefix("captured schedule does not match effective control schedule"))
	}
	if evidence.Before == nil || evidence.After == nil {
		if evidence.Before == nil {
			reasons = append(reasons, prefix("before snapshot is missing"))
		}
		if evidence.After == nil {
			reasons = append(reasons, prefix("after snapshot is missing"))
		}
		return dedupeStrings(reasons)
	}
	before, after := *evidence.Before, *evidence.After
	reasons = append(reasons, validateQdiscSnapshotBounds(evidence.Schedule, before, after, prefix)...)

	computed := netops.DeltaQdiscPair(before, after)
	for _, reason := range computed.Errors {
		reasons = append(reasons, prefix(reason))
	}
	if evidence.Delta == nil {
		reasons = append(reasons, prefix("counter delta is missing"))
	} else {
		for _, reason := range evidence.Delta.Errors {
			reasons = append(reasons, prefix("stored delta: "+reason))
		}
		if !sameQdiscDelta(evidence.Delta.ServerEgress, computed.ServerEgress) ||
			!sameQdiscDelta(evidence.Delta.ClientEgress, computed.ClientEgress) {
			reasons = append(reasons, prefix("stored counter delta does not match snapshots"))
		}
	}

	pair := cfg.Netem.pairSpec()
	checks := []struct {
		name      string
		namespace string
		device    string
		expected  netops.Netem
		before    netops.QdiscSample
		after     netops.QdiscSample
		delta     *netops.QdiscCounterDelta
	}{
		{"server_egress", pair.ServerNS, pair.ServerVeth, cfg.Netem.ServerEgress, before.ServerEgress, after.ServerEgress, computed.ServerEgress},
		{"client_egress", pair.ClientNS, pair.ClientVeth, cfg.Netem.ClientEgress, before.ClientEgress, after.ClientEgress, computed.ClientEgress},
	}
	for _, check := range checks {
		phases := []struct {
			name   string
			sample netops.QdiscSample
		}{{"before", check.before}, {"after", check.after}}
		for _, phase := range phases {
			sample := phase.sample
			if sample.Namespace != check.namespace || sample.Device != check.device {
				reasons = append(reasons, prefix(fmt.Sprintf("%s %s source=%s/%s, want %s/%s",
					check.name, phase.name, sample.Namespace, sample.Device, check.namespace, check.device)))
			}
			if err := netops.ValidateQdiscSampleReadback(sample); err != nil {
				reasons = append(reasons, prefix(fmt.Sprintf("%s %s readback mismatch: %v", check.name, phase.name, err)))
			}
			if sample.Stats == nil {
				continue
			}
			if err := netops.ValidateNetemEcho(check.expected, *sample.Stats); err != nil {
				reasons = append(reasons, prefix(fmt.Sprintf("%s %s qdisc mismatch: %v", check.name, phase.name, err)))
			}
		}
		if check.expected.LossPercent > 0 {
			if check.delta == nil {
				reasons = append(reasons, prefix(check.name+" counter delta unavailable for configured loss"))
			} else if check.delta.Dropped == 0 {
				reasons = append(reasons, prefix(check.name+" observed dropped delta is zero for configured loss"))
			}
		}
	}
	return dedupeStrings(reasons)
}

// validateDeterministicLossEvidence requires known-packet accounting: the
// losstrace counter samples must sit wholly inside the effective measurement
// window, and the regenerated trace must drop at least one packet in the
// sampled range for every seeded direction.
func validateDeterministicLossEvidence(cfg *RunConfig, controlResult *control.Result, evidence *NetemLossEvidence, prefix func(string) string) []string {
	if evidence == nil {
		return []string{prefix("missing for configured deterministic loss")}
	}
	var reasons []string
	if evidence.Version != 1 {
		reasons = append(reasons, prefix(fmt.Sprintf("version=%d, want 1", evidence.Version)))
	}
	if evidence.Mode != lossEvidenceModeDeterministicTrace {
		reasons = append(reasons, prefix(fmt.Sprintf("mode=%q, want %q", evidence.Mode, lossEvidenceModeDeterministicTrace)))
	}
	if !evidence.Supported {
		reasons = append(reasons, prefix("deterministic trace accounting marked unsupported"))
	}
	if evidence.Scope != lossEvidenceScopeEffectiveInner {
		reasons = append(reasons, prefix(fmt.Sprintf("scope=%q does not prove in-window loss", evidence.Scope)))
	}
	for _, reason := range evidence.Errors {
		reasons = append(reasons, prefix(reason))
	}
	if controlResult == nil {
		reasons = append(reasons, prefix("control schedule is unavailable"))
	} else if evidence.Schedule != controlResult.Schedule {
		reasons = append(reasons, prefix("captured schedule does not match effective control schedule"))
	}
	det := evidence.Deterministic
	if det == nil {
		reasons = append(reasons, prefix("deterministic counter samples are missing"))
		return dedupeStrings(reasons)
	}
	if det.CaptureBeforeStartNS < evidence.Schedule.StartAtNS {
		reasons = append(reasons, prefix(fmt.Sprintf("before sample started outside measurement window: %d < %d", det.CaptureBeforeStartNS, evidence.Schedule.StartAtNS)))
	}
	if det.CaptureAfterFinishNS > evidence.Schedule.StopAtNS {
		reasons = append(reasons, prefix(fmt.Sprintf("after sample finished outside measurement window: %d > %d", det.CaptureAfterFinishNS, evidence.Schedule.StopAtNS)))
	}
	if det.CaptureBeforeFinishNS >= det.CaptureAfterStartNS {
		reasons = append(reasons, prefix("counter samples overlap"))
	}

	pair := cfg.Netem.pairSpec()
	checks := []struct {
		name      string
		namespace string
		device    string
		expected  netops.Netem
		record    *DeterministicLossDirection
	}{
		{"server_egress", pair.ServerNS, pair.ServerVeth, cfg.Netem.ServerEgress, det.ServerEgress},
		{"client_egress", pair.ClientNS, pair.ClientVeth, cfg.Netem.ClientEgress, det.ClientEgress},
	}
	for _, check := range checks {
		if check.expected.LossPercent <= 0 {
			continue
		}
		if check.expected.LossSeed == 0 {
			reasons = append(reasons, prefix(check.name+" mixes random loss into a deterministic run"))
			continue
		}
		record := check.record
		if record == nil {
			reasons = append(reasons, prefix(check.name+" counter record is missing for configured loss"))
			continue
		}
		if record.Namespace != check.namespace || record.Dev != check.device {
			reasons = append(reasons, prefix(fmt.Sprintf("%s source=%s/%s, want %s/%s",
				check.name, record.Namespace, record.Dev, check.namespace, check.device)))
		}
		if record.TraceBits != traceBitsOrDefault(check.expected) {
			reasons = append(reasons, prefix(fmt.Sprintf("%s trace_bits=%d does not match configured %d",
				check.name, record.TraceBits, traceBitsOrDefault(check.expected))))
		}
		if record.CounterAfter < record.CounterBefore {
			reasons = append(reasons, prefix(check.name+" counter went backwards"))
			continue
		}
		if record.Packets != record.CounterAfter-record.CounterBefore {
			reasons = append(reasons, prefix(check.name+" stored packet count does not match counters"))
		}
		if record.Packets == 0 {
			reasons = append(reasons, prefix(check.name+" observed no packets in the measurement window"))
		}
		if record.ExpectedDrops == 0 {
			reasons = append(reasons, prefix(check.name+" expected drop count is zero for configured loss"))
		}
		words, realized, err := losstrace.Generate(check.expected.LossSeed, check.expected.LossPercent,
			check.expected.LossBurstLen, record.TraceBits)
		if err != nil {
			reasons = append(reasons, prefix(fmt.Sprintf("%s trace regeneration failed: %v", check.name, err)))
			continue
		}
		if HashValue(words) != record.TraceSHA256 {
			reasons = append(reasons, prefix(check.name+" stored trace hash does not match the regenerated trace"))
		}
		if realized != record.RealizedLossPct {
			reasons = append(reasons, prefix(check.name+" stored realized loss rate does not match the regenerated trace"))
		}
		if drops, countErr := losstrace.CountDropsInRange(words, record.CounterBefore, record.CounterAfter); countErr != nil {
			reasons = append(reasons, prefix(fmt.Sprintf("%s trace drop recount failed: %v", check.name, countErr)))
		} else if drops != record.ExpectedDrops {
			reasons = append(reasons, prefix(check.name+" stored expected drops do not match the regenerated trace"))
		}
	}
	return dedupeStrings(reasons)
}

func validateQdiscSnapshotBounds(schedule control.ScheduleMessage, before, after netops.QdiscPairSnapshot, prefix func(string) string) []string {
	var reasons []string
	if before.CaptureStartNS < schedule.StartAtNS {
		reasons = append(reasons, prefix(fmt.Sprintf("before snapshot started outside measurement window: %d < %d", before.CaptureStartNS, schedule.StartAtNS)))
	}
	if before.CaptureFinishNS < before.CaptureStartNS {
		reasons = append(reasons, prefix("before snapshot timing is inverted"))
	}
	if before.CaptureFinishNS > after.CaptureStartNS {
		reasons = append(reasons, prefix("qdisc snapshot intervals overlap or are reversed"))
	}
	if after.CaptureStartNS < schedule.StartAtNS || after.CaptureFinishNS < after.CaptureStartNS {
		reasons = append(reasons, prefix("after snapshot timing is outside or inverted"))
	}
	if after.CaptureFinishNS > schedule.StopAtNS {
		reasons = append(reasons, prefix(fmt.Sprintf("after snapshot finished outside measurement window: %d > %d", after.CaptureFinishNS, schedule.StopAtNS)))
	}
	for _, sample := range []netops.QdiscSample{before.ServerEgress, before.ClientEgress, after.ServerEgress, after.ClientEgress} {
		if sample.CaptureStartNS < schedule.StartAtNS || sample.CaptureFinishNS > schedule.StopAtNS ||
			sample.CaptureFinishNS < sample.CaptureStartNS {
			reasons = append(reasons, prefix(fmt.Sprintf("%s/%s sample timing is outside or inverted", sample.Namespace, sample.Device)))
		}
	}
	for _, pair := range []struct {
		name     string
		snapshot netops.QdiscPairSnapshot
	}{{"before", before}, {"after", after}} {
		snapshot := pair.snapshot
		if snapshot.CaptureStartNS > snapshot.ServerEgress.CaptureStartNS ||
			snapshot.ServerEgress.CaptureStartNS > snapshot.ServerEgress.CaptureFinishNS ||
			snapshot.ServerEgress.CaptureFinishNS > snapshot.ClientEgress.CaptureStartNS ||
			snapshot.ClientEgress.CaptureStartNS > snapshot.ClientEgress.CaptureFinishNS ||
			snapshot.ClientEgress.CaptureFinishNS > snapshot.CaptureFinishNS {
			reasons = append(reasons, prefix(pair.name+" pair bracket does not enclose sequential server/client samples"))
		}
	}
	return reasons
}

func sameQdiscDelta(a, b *netops.QdiscCounterDelta) bool {
	if a == nil || b == nil {
		return a == nil && b == nil
	}
	return *a == *b
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
	PeerDisconnects       *uint64                `json:"peer_disconnects"`
	AuthoritativeProgress *authoritativeProgress `json:"authoritative_progress"`
}

// totalPeerDisconnects sums the run-time disconnect disclosures across all
// participants. Only clients that survive a peer disconnect report the key;
// absence means zero.
func totalPeerDisconnects(controlResult *control.Result) uint64 {
	if controlResult == nil {
		return 0
	}
	var total uint64
	for _, participant := range controlResult.Participants {
		var stats scenarioDoneStats
		if len(participant.Done.Stats) == 0 ||
			json.Unmarshal(participant.Done.Stats, &stats) != nil {
			continue
		}
		if stats.PeerDisconnects != nil {
			total += *stats.PeerDisconnects
		}
	}
	return total
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
