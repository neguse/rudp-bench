package run

import (
	"encoding/json"
	"fmt"
	"strings"
	"testing"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/control"
	"github.com/neguse/rudp-bench/orchestrator/netops"
)

func TestEvaluateGateValid(t *testing.T) {
	gate := EvaluateGate(GateInput{
		Control:            controlResultWithMargin(10),
		Metrics:            gateMetrics(100, 99),
		Processes:          []ProcessResult{{Role: "server", ProcIndex: -1, PID: 10, Exited: true, ExitCode: 0}},
		AttemptedThreshold: 0.99,
		NetemEnabled:       true,
		UDPDropDelta:       netops.UDPStats{},
	})
	if gate.Verdict != VerdictValid {
		t.Fatalf("verdict = %s, reasons=%v", gate.Verdict, gate.Reasons)
	}
}

func TestScenarioDoneStatsSeparateInvalidFromSUTFailure(t *testing.T) {
	scenario := authoritativeFixture()
	cfg := RunConfig{Scenario: &scenario, TotalConns: 2, Duration: Duration{Duration: time.Second}}
	server := control.Participant{
		Hello: control.HelloMessage{Role: "server", ProcIndex: -1}, Ready: control.ReadyMessage{Conns: 0},
		Done: control.DoneMessage{Stats: json.RawMessage(`{"invalid_payload":0,"authoritative_progress":{"role":"server","local_conns":0,"roster_conns":2,"input_last_sent_min":0,"input_last_sent_max":0,"state_header_seq_recv_min":0,"state_header_seq_recv_max":0,"state_applied_input_seq_recv_min":0,"state_applied_input_seq_recv_max":0,"server_state_ticks":20}}`)},
	}
	client := control.Participant{
		Hello: control.HelloMessage{Role: "client", ProcIndex: 0}, Ready: control.ReadyMessage{Conns: 2},
		Done: control.DoneMessage{Stats: json.RawMessage(`{"invalid_payload":0,"authoritative_progress":{"role":"client","local_conns":2,"roster_conns":2,"input_last_sent_min":60,"input_last_sent_max":60,"state_header_seq_recv_min":20,"state_header_seq_recv_max":20,"state_applied_input_seq_recv_min":59,"state_applied_input_seq_recv_max":60,"server_state_ticks":0}}`)},
	}
	invalid, failed := validateScenarioDoneStats(&control.Result{Participants: []control.Participant{server, client}}, cfg)
	if len(invalid) != 0 || len(failed) != 0 {
		t.Fatalf("invalid=%v failed=%v", invalid, failed)
	}
	badClient := client
	badClient.Done.Stats = json.RawMessage(`{"invalid_payload":1,"authoritative_progress":{"role":"client","local_conns":2,"roster_conns":2}}`)
	invalid, failed = validateScenarioDoneStats(&control.Result{Participants: []control.Participant{server, badClient}}, cfg)
	if len(invalid) != 0 || !reasonsContain(failed, "payload corruption") || !reasonsContain(failed, "progress") {
		t.Fatalf("invalid=%v failed=%v", invalid, failed)
	}
	missing := client
	missing.Done.Stats = json.RawMessage(`{}`)
	invalid, _ = validateScenarioDoneStats(&control.Result{Participants: []control.Participant{server, missing}}, cfg)
	if !reasonsContain(invalid, "missing invalid_payload") || !reasonsContain(invalid, "missing authoritative_progress") {
		t.Fatalf("invalid=%v", invalid)
	}
}

func TestEvaluateGateInvalidReasons(t *testing.T) {
	gate := EvaluateGate(GateInput{
		Control:            controlResultWithMargin(-1),
		Metrics:            gateMetrics(100, 98),
		Processes:          []ProcessResult{{Role: "client", ProcIndex: 0, PID: 20, Exited: true, ExitCode: 7, Error: "exit status 7"}},
		AttemptedThreshold: 0.99,
		NetemEnabled:       true,
		UDPDropDelta:       netops.UDPStats{InErrors: 1, RcvbufErrors: 2},
	})
	if gate.Verdict != VerdictInvalid {
		t.Fatalf("verdict = %s, want INVALID", gate.Verdict)
	}
	for _, want := range []string{
		"negative schedule margin",
		"attempted_ratio",
		"exit_code=7",
		"UDP drop delta",
	} {
		if !reasonsContain(gate.Reasons, want) {
			t.Fatalf("reasons %v do not contain %q", gate.Reasons, want)
		}
	}
}

func TestEvaluateGateRejectsTimestampOrderViolations(t *testing.T) {
	metrics := gateMetrics(100, 100)
	metrics.Raw.TimestampOrderViolations = 2
	gate := EvaluateGate(GateInput{
		Control:            controlResultWithMargin(10),
		Metrics:            metrics,
		Processes:          []ProcessResult{{Role: "server", ProcIndex: -1, PID: 10, Exited: true, ExitCode: 0}},
		AttemptedThreshold: 0.99,
	})
	if gate.Verdict != VerdictInvalid || !reasonsContain(gate.Reasons, "timestamp_order_violations=2, want 0") {
		t.Fatalf("gate = %+v", gate)
	}
}

func TestEvaluateGateServerUDPDropIsDisclosureOnly(t *testing.T) {
	gate := EvaluateGate(GateInput{
		Control:            controlResultWithMargin(10),
		Metrics:            gateMetrics(100, 100),
		Processes:          []ProcessResult{{Role: "server", ProcIndex: -1, PID: 10, Exited: true, ExitCode: 0}},
		AttemptedThreshold: 0.99,
		NetemEnabled:       true,
		ServerUDPDropDelta: netops.UDPStats{InErrors: 3, RcvbufErrors: 2},
	})
	if gate.Verdict != VerdictValid {
		t.Fatalf("server receive drop must not invalidate the measurement: %+v", gate)
	}
	if len(gate.SUTFailureReasons) != 0 {
		t.Fatalf("server receive drop must not break the run by itself (goal metrics decide): %+v", gate)
	}
}

func TestEvaluateGateRequiresRequestedOffloadEvidence(t *testing.T) {
	cfg := RunConfig{Netem: &NetemRegime{DisableOffloads: true}}
	gate := EvaluateGate(GateInput{
		Control:            controlResultWithMargin(10),
		Metrics:            gateMetrics(100, 100),
		Processes:          []ProcessResult{{Role: "server", ProcIndex: -1, PID: 10, Exited: true, ExitCode: 0}},
		AttemptedThreshold: 0.99,
		NetemEnabled:       true,
		Config:             &cfg,
	})
	if gate.Verdict != VerdictInvalid || !reasonsContain(gate.Reasons, "offload evidence: missing") {
		t.Fatalf("gate = %+v", gate)
	}

	cfg.Netem.DisableOffloads = false
	gate = EvaluateGate(GateInput{
		Control:            controlResultWithMargin(10),
		Metrics:            gateMetrics(100, 100),
		Processes:          []ProcessResult{{Role: "server", ProcIndex: -1, PID: 10, Exited: true, ExitCode: 0}},
		AttemptedThreshold: 0.99,
		NetemEnabled:       true,
		Config:             &cfg,
	})
	if gate.Verdict != VerdictValid {
		t.Fatalf("disabled offload gate changed legacy behavior: %+v", gate)
	}
}

func TestEvaluateGateRequiresPostMeasurementOffloadEvidence(t *testing.T) {
	cfg := RunConfig{Netem: &NetemRegime{DisableOffloads: true, LinkMTUBytes: 1500}}
	before := validGateOffloadEvidence(cfg.Netem.pairSpec())
	input := GateInput{
		Control:            controlResultWithMargin(10),
		Metrics:            gateMetrics(100, 100),
		Processes:          []ProcessResult{{Role: "server", ProcIndex: -1, PID: 10, Exited: true, ExitCode: 0}},
		AttemptedThreshold: 0.99,
		NetemEnabled:       true,
		Config:             &cfg,
		Offloads:           &before,
	}
	gate := EvaluateGate(input)
	if gate.Verdict != VerdictInvalid || !reasonsContain(gate.Reasons, "after measurement: missing") {
		t.Fatalf("missing post-measurement readback accepted: %+v", gate)
	}
	input.OffloadsAfter = &before
	gate = EvaluateGate(input)
	if gate.Verdict != VerdictValid {
		t.Fatalf("valid before/after readback rejected: %+v", gate)
	}
}

func validGateOffloadEvidence(pair netops.PairSpec) netops.OffloadEvidence {
	var raw strings.Builder
	raw.WriteString("Features for benchmark-veth:\n")
	features := map[string]netops.OffloadFeatureState{}
	for _, name := range netops.RequiredOffloadFeatures() {
		raw.WriteString(name + ": off\n")
		features[name] = netops.OffloadFeatureState{}
	}
	sample := func(namespace, device string) netops.OffloadInterfaceEvidence {
		return netops.OffloadInterfaceEvidence{
			Namespace: namespace, Device: device, LinkMTUBytes: pair.LinkMTUBytes,
			Features: features, Raw: raw.String(),
			LinkRaw: `[{"ifname":"` + device + `","mtu":1500}]`,
		}
	}
	evidence := netops.OffloadEvidence{
		Version: netops.OffloadEvidenceVersion, RequiredFeatures: netops.RequiredOffloadFeatures(),
		Server: sample(pair.ServerNS, pair.ServerVeth), Client: sample(pair.ClientNS, pair.ClientVeth),
	}
	evidence.SHA256 = netops.HashOffloadEvidence(evidence)
	return evidence
}

func TestValidateNetemLossEvidenceRequiresDirectionalInWindowDrops(t *testing.T) {
	cfg, controlResult, evidence := validNetemLossEvidenceFixture()
	if reasons := ValidateNetemLossEvidence(&cfg, controlResult, evidence); len(reasons) != 0 {
		t.Fatalf("valid evidence rejected: %v", reasons)
	}

	serverAfter := *evidence.After.ServerEgress.Stats
	serverAfter.Dropped = evidence.Before.ServerEgress.Stats.Dropped
	evidence.After.ServerEgress.Stats = &serverAfter
	delta := netops.DeltaQdiscPair(*evidence.Before, *evidence.After)
	evidence.Delta = &delta
	reasons := ValidateNetemLossEvidence(&cfg, controlResult, evidence)
	if !reasonsContain(reasons, "server_egress observed dropped delta is zero") {
		t.Fatalf("zero directional drop reasons = %v", reasons)
	}
}

func TestValidateNetemLossEvidenceRejectsUnreadableOrOutOfWindowSnapshots(t *testing.T) {
	cfg, controlResult, evidence := validNetemLossEvidenceFixture()
	evidence.Before.CaptureStartNS = controlResult.Schedule.StartAtNS - 1
	evidence.After.ClientEgress.Error = "permission denied"
	evidence.After.ClientEgress.Stats = nil
	delta := netops.DeltaQdiscPair(*evidence.Before, *evidence.After)
	evidence.Delta = &delta
	reasons := ValidateNetemLossEvidence(&cfg, controlResult, evidence)
	for _, want := range []string{"outside measurement window", "permission denied", "client_egress counter delta unavailable"} {
		if !reasonsContain(reasons, want) {
			t.Fatalf("reasons %v do not contain %q", reasons, want)
		}
	}
}

func TestValidateNetemLossEvidenceRejectsSamplesOutsidePairBracket(t *testing.T) {
	cfg, controlResult, evidence := validNetemLossEvidenceFixture()
	evidence.Before.ClientEgress.CaptureStartNS = evidence.Before.ServerEgress.CaptureFinishNS - 1
	reasons := ValidateNetemLossEvidence(&cfg, controlResult, evidence)
	if !reasonsContain(reasons, "pair bracket does not enclose sequential server/client samples") {
		t.Fatalf("inconsistent pair/sample timing reasons = %v", reasons)
	}
}

func TestValidateNetemLossEvidenceRejectsRawStatsDrift(t *testing.T) {
	cfg, controlResult, evidence := validNetemLossEvidenceFixture()
	evidence.Before.ClientEgress.Raw = strings.Replace(
		evidence.Before.ClientEgress.Raw, "loss random 2%", "loss random 2% 25%", 1)
	reasons := ValidateNetemLossEvidence(&cfg, controlResult, evidence)
	if !reasonsContain(reasons, "stored stats do not match raw tc readback") {
		t.Fatalf("raw/stats drift reasons = %v", reasons)
	}
}

func TestValidateNetemLossEvidenceOnlyRequiresDropOnConfiguredDirection(t *testing.T) {
	cfg, controlResult, evidence := validNetemLossEvidenceFixture()
	cfg.Netem.ServerEgress = netops.Netem{}
	serverBefore := netops.QdiscStats{Kind: "noqueue", Root: true, SentPackets: 10}
	serverAfter := netops.QdiscStats{Kind: "noqueue", Root: true, SentPackets: 20}
	evidence.Before.ServerEgress.Stats = &serverBefore
	evidence.After.ServerEgress.Stats = &serverAfter
	evidence.Before.ServerEgress.Raw = qdiscFixtureRaw(serverBefore)
	evidence.After.ServerEgress.Raw = qdiscFixtureRaw(serverAfter)
	delta := netops.DeltaQdiscPair(*evidence.Before, *evidence.After)
	evidence.Delta = &delta
	if reasons := ValidateNetemLossEvidence(&cfg, controlResult, evidence); len(reasons) != 0 {
		t.Fatalf("unconfigured server direction required a drop: %v", reasons)
	}
}

func TestValidateNetemLossEvidenceRejectsMixedDeterministicRun(t *testing.T) {
	cfg, controlResult, _ := validNetemLossEvidenceFixture()
	// client は seed 済み、server は random のままの混在 run は
	// deterministic 会計で server 側を証明できない
	cfg.Netem.ClientEgress.LossSeed = 99
	reasons := ValidateNetemLossEvidence(&cfg, controlResult, nil)
	if !reasonsContain(reasons, "missing for configured deterministic loss") {
		t.Fatalf("deterministic evidence reasons = %v", reasons)
	}
}

func validNetemLossEvidenceFixture() (RunConfig, *control.Result, *NetemLossEvidence) {
	schedule := control.ScheduleMessage{Type: control.TypeSchedule, StartAtNS: 100, StopAtNS: 1_000, DrainUntilNS: 1_100}
	cfg := RunConfig{Netem: &NetemRegime{
		ServerNS: "srv", ClientNS: "cli", ServerVeth: "vs", ClientVeth: "vc",
		ServerEgress: netops.Netem{LossPercent: 1},
		ClientEgress: netops.Netem{LossPercent: 2},
	}}
	serverBefore := netops.QdiscStats{Kind: "netem", Root: true, Limit: 10_000, LossPercent: 1, SentPackets: 100, Dropped: 4, Overlimits: 2}
	clientBefore := netops.QdiscStats{Kind: "netem", Root: true, Limit: 10_000, LossPercent: 2, SentPackets: 200, Dropped: 8, Overlimits: 3}
	serverAfter := serverBefore
	serverAfter.SentPackets = 180
	serverAfter.Dropped = 5
	serverAfter.Overlimits = 4
	clientAfter := clientBefore
	clientAfter.SentPackets = 350
	clientAfter.Dropped = 11
	clientAfter.Overlimits = 7
	before := netops.QdiscPairSnapshot{
		CaptureStartNS:  110,
		CaptureFinishNS: 160,
		ServerEgress:    netops.QdiscSample{Namespace: "srv", Device: "vs", CaptureStartNS: 110, CaptureFinishNS: 130, Stats: &serverBefore, Raw: qdiscFixtureRaw(serverBefore)},
		ClientEgress:    netops.QdiscSample{Namespace: "cli", Device: "vc", CaptureStartNS: 135, CaptureFinishNS: 155, Stats: &clientBefore, Raw: qdiscFixtureRaw(clientBefore)},
	}
	after := netops.QdiscPairSnapshot{
		CaptureStartNS:  800,
		CaptureFinishNS: 850,
		ServerEgress:    netops.QdiscSample{Namespace: "srv", Device: "vs", CaptureStartNS: 800, CaptureFinishNS: 820, Stats: &serverAfter, Raw: qdiscFixtureRaw(serverAfter)},
		ClientEgress:    netops.QdiscSample{Namespace: "cli", Device: "vc", CaptureStartNS: 825, CaptureFinishNS: 845, Stats: &clientAfter, Raw: qdiscFixtureRaw(clientAfter)},
	}
	delta := netops.DeltaQdiscPair(before, after)
	return cfg, &control.Result{Schedule: schedule}, &NetemLossEvidence{
		Version: 1, Mode: lossEvidenceModeRandomNetem, Supported: true,
		Scope: lossEvidenceScopeEffectiveInner, Schedule: schedule,
		Before: &before, After: &after, Delta: &delta,
	}
}

func qdiscFixtureRaw(stats netops.QdiscStats) string {
	return fmt.Sprintf("qdisc %s 1: root limit %d loss random %g%%\nSent %d bytes %d pkt (dropped %d, overlimits %d requeues %d)\n",
		stats.Kind, stats.Limit, stats.LossPercent, stats.SentBytes, stats.SentPackets,
		stats.Dropped, stats.Overlimits, stats.Requeues)
}

func TestEvaluateGateAttemptedShortfallWithDisconnectsIsSUTFailure(t *testing.T) {
	controlResult := controlResultWithMargin(10)
	controlResult.Participants[0].Done.Stats = []byte(`{"invalid_payload":0,"peer_disconnects":8}`)
	gate := EvaluateGate(GateInput{
		Control:            controlResult,
		Metrics:            gateMetrics(100, 80),
		Processes:          []ProcessResult{{Role: "server", ProcIndex: -1, PID: 10, Exited: true, ExitCode: 0}},
		AttemptedThreshold: 0.99,
	})
	if gate.Verdict != VerdictValid {
		t.Fatalf("verdict = %s, reasons=%v", gate.Verdict, gate.Reasons)
	}
	if !reasonsContain(gate.SUTFailureReasons, "peer_disconnects=8") {
		t.Fatalf("SUT reasons %v do not attribute the shortfall to disconnects", gate.SUTFailureReasons)
	}
}

func TestEvaluateGateAttemptedShortfallWithoutDisconnectsStaysInvalid(t *testing.T) {
	gate := EvaluateGate(GateInput{
		Control:            controlResultWithMargin(10),
		Metrics:            gateMetrics(100, 80),
		Processes:          []ProcessResult{{Role: "server", ProcIndex: -1, PID: 10, Exited: true, ExitCode: 0}},
		AttemptedThreshold: 0.99,
	})
	if gate.Verdict != VerdictInvalid || !reasonsContain(gate.Reasons, "attempted_ratio") {
		t.Fatalf("gate = %+v", gate)
	}
}

func controlResultWithMargin(margin int64) *control.Result {
	return &control.Result{
		Valid: true,
		Participants: []control.Participant{{
			Hello:       control.HelloMessage{PID: 123, ProcIndex: 0},
			SchedAck:    control.SchedAckMessage{MarginNS: margin},
			AckReceived: true,
		}},
	}
}

func gateMetrics(slots, submitted uint64) *MergedMetrics {
	return &MergedMetrics{
		Classes: map[string]ClassAggregate{
			ClassLossTolerant: {ClassCounts: ClassCounts{Slots: slots, Submitted: submitted}},
			ClassMustDeliver:  {},
		},
	}
}

func reasonsContain(reasons []string, needle string) bool {
	for _, reason := range reasons {
		if strings.Contains(reason, needle) {
			return true
		}
	}
	return false
}
