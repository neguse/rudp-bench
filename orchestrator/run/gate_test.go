package run

import (
	"encoding/json"
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

func TestEvaluateGateServerUDPDropIsSUTFailure(t *testing.T) {
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
	if !reasonsContain(gate.SUTFailureReasons, "server netns UDP drop delta") {
		t.Fatalf("server receive drop must be attributed to the SUT: %+v", gate)
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
