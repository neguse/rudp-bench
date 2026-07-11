package run

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/neguse/rudp-bench/orchestrator/netops"
)

func authoritativeFixture() ScenarioSpec {
	return ScenarioSpec{
		Name: "authoritative-test",
		Kind: ScenarioAuthoritativeState,
		ClientInput: &TrafficSpec{
			TrafficID:    TrafficIDClientInput,
			LossTolerant: TrafficClassSpec{RateHz: 60, PayloadBytes: 64, MinDeliveryRatio: 0.99},
			MustDeliver: TrafficClassSpec{RateHz: 1, PayloadBytes: 64, DeadlineNS: 100_000_000,
				MinDeadlineHitRatio: 0.99, MinEventualDeliveryRatio: 0.99},
		},
		ServerState: &TrafficSpec{
			TrafficID:    TrafficIDServerState,
			LossTolerant: TrafficClassSpec{RateHz: 20, PayloadBytes: 256, StalenessP99NS: 100_000_000},
			MustDeliver: TrafficClassSpec{RateHz: 1, PayloadBytes: 64, DeadlineNS: 100_000_000,
				MinDeadlineHitRatio: 0.99, MinEventualDeliveryRatio: 0.99},
		},
	}
}

func TestScenarioAllowsDiagnosticSLOsButRejectsDisabledClassSettings(t *testing.T) {
	s := authoritativeFixture()
	s.ClientInput.LossTolerant.MinDeliveryRatio = 0
	if err := s.Validate(); err != nil {
		t.Fatalf("diagnostic scenario should be valid: %v", err)
	}
	if s.HasCompletePrimarySLOs() || len(s.MissingPrimarySLOs()) != 1 {
		t.Fatalf("missing primary SLOs = %v", s.MissingPrimarySLOs())
	}
	s = authoritativeFixture()
	s.ClientInput.MustDeliver.RateHz = 0
	if err := s.Validate(); err == nil || !strings.Contains(err.Error(), "disabled") {
		t.Fatalf("disabled class settings error = %v", err)
	}
	s = authoritativeFixture()
	s.Name = "../escape"
	if err := s.Validate(); err == nil || !strings.Contains(err.Error(), "path-safe") {
		t.Fatalf("unsafe name error = %v", err)
	}
}

func TestLoadConfigRejectsUnknownScenarioField(t *testing.T) {
	path := filepath.Join(t.TempDir(), "config.json")
	data := []byte(`{
  "transport":"raw","server_command":["server"],"client_command":["client"],
  "client_procs":1,"total_conns":1,"output_dir":"out",
  "scenario":{"name":"baseline","kind":"environment_baseline","client_input":{
    "traffic_id":1,
    "loss_tolerant":{"rate_hz":1,"payload_bytes":32,"stalenes_p99_ns":1000},
    "must_deliver":{"rate_hz":0,"payload_bytes":0}
  }}
}`)
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := LoadConfig(path); err == nil || !strings.Contains(err.Error(), "unknown field") {
		t.Fatalf("unknown scenario field error = %v", err)
	}
}

func TestScenarioAuthoritativeValidation(t *testing.T) {
	s := authoritativeFixture()
	if err := s.Validate(); err != nil {
		t.Fatal(err)
	}

	s.ServerState.TrafficID = s.ClientInput.TrafficID
	if err := s.Validate(); err == nil || !strings.Contains(err.Error(), "traffic_id must differ") {
		t.Fatalf("expected duplicate traffic id error, got %v", err)
	}
}

func TestScenarioRequiresStateMetadataSpace(t *testing.T) {
	s := authoritativeFixture()
	s.ServerState.LossTolerant.PayloadBytes = 32
	if err := s.Validate(); err == nil || !strings.Contains(err.Error(), "must be >= 40") {
		t.Fatalf("expected payload size error, got %v", err)
	}
}

func TestScenarioCommonArgs(t *testing.T) {
	args := strings.Join(authoritativeFixture().CommonArgs(128), " ")
	for _, want := range []string{
		"--scenario authoritative_state",
		"--total-conns 128",
		"--input-traffic-id 1",
		"--input-rate-lt 60",
		"--state-traffic-id 2",
		"--state-rate-lt 20",
		"--state-payload-lt 256",
	} {
		if !strings.Contains(args, want) {
			t.Fatalf("args %q missing %q", args, want)
		}
	}
}

func TestScenarioLinkPPS(t *testing.T) {
	client, server := authoritativeFixture().LinkPPS(10)
	if client != 610 || server != 210 {
		t.Fatalf("authoritative link pps = (%g, %g), want (610, 210)", client, server)
	}

	relay := ScenarioSpec{
		Name: "relay-test",
		Kind: ScenarioRoomRelay,
		RoomPublish: &TrafficSpec{
			TrafficID:    3,
			LossTolerant: TrafficClassSpec{RateHz: 20, PayloadBytes: 128},
			MustDeliver:  TrafficClassSpec{RateHz: 1, PayloadBytes: 64},
		},
	}
	client, server = relay.LinkPPS(10)
	if client != 210 || server != 2100 {
		t.Fatalf("relay link pps = (%g, %g), want (210, 2100)", client, server)
	}
}

func TestScenarioSteadyExpectedRates(t *testing.T) {
	cfg := RunConfig{
		Scenario:     ptr(authoritativeFixture()),
		TotalConns:   10,
		SteadyWarmup: true,
		Netem: &NetemRegime{
			ClientEgress: netops.Netem{LossPercent: 1},
			ServerEgress: netops.Netem{LossPercent: 10},
		},
	}
	sent, recv := steadyExpectedRates(cfg)
	if sent != 61 || recv != 19 {
		t.Fatalf("steady expected = (%g, %g), want (61, 19)", sent, recv)
	}
}

func ptr[T any](v T) *T { return &v }
