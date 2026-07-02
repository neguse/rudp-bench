package run

import (
	"strings"
	"testing"

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
