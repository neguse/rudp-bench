package run

import (
	"math"
	"testing"

	"github.com/neguse/rudp-bench/orchestrator/control"
	"github.com/neguse/rudp-bench/orchestrator/netops"
	"github.com/neguse/rudp-bench/orchestrator/sampler"
)

func costTestResult() *Result {
	scenario := &ScenarioSpec{
		Name: "cost-test", Kind: ScenarioAuthoritativeState,
		ClientInput: &TrafficSpec{
			TrafficID:    TrafficIDClientInput,
			LossTolerant: TrafficClassSpec{RateHz: 30, PayloadBytes: 64, MinDeliveryRatio: 0.9},
		},
		ServerState: &TrafficSpec{
			TrafficID:    TrafficIDServerState,
			LossTolerant: TrafficClassSpec{RateHz: 20, PayloadBytes: 1000, MinDeliveryRatio: 0.9},
		},
	}
	return &Result{
		Config: RunConfig{TotalConns: 4, Scenario: scenario},
		Control: &control.Result{Schedule: control.ScheduleMessage{
			StartAtNS: 1_000_000_000, StopAtNS: 11_000_000_000,
		}},
		Processes: []ProcessResult{
			{Role: "server", PID: 100},
			{Role: "client", PID: 200},
			{Role: "client", PID: 201},
		},
		Samples: []sampler.Series{
			{PID: 100, Samples: []sampler.Sample{
				{TimeNS: 1_000_000_000, CPUTimeNS: 500_000_000, RSSBytes: 10 << 20},
				{TimeNS: 11_000_000_000, CPUTimeNS: 2_500_000_000, RSSBytes: 12 << 20},
			}},
			{PID: 200, Samples: []sampler.Sample{
				{TimeNS: 1_000_000_000, CPUTimeNS: 0, RSSBytes: 5 << 20},
				{TimeNS: 11_000_000_000, CPUTimeNS: 1_000_000_000, RSSBytes: 5 << 20},
			}},
			{PID: 201, Samples: []sampler.Sample{
				{TimeNS: 1_000_000_000, CPUTimeNS: 0, RSSBytes: 6 << 20},
				{TimeNS: 11_000_000_000, CPUTimeNS: 500_000_000, RSSBytes: 6 << 20},
			}},
		},
		Metrics: &MergedMetrics{
			Classes: map[string]ClassAggregate{
				ClassLossTolerant: {ClassCounts: ClassCounts{DeliveredUnique: 2000}},
			},
			Traffic: []TrafficAggregate{
				{TrafficID: TrafficIDClientInput, Direction: DirectionClientToServer,
					Class: ClassLossTolerant, ClassCounts: ClassCounts{Submitted: 1200}},
				{TrafficID: TrafficIDServerState, Direction: DirectionServerToClient,
					Class: ClassLossTolerant, ClassCounts: ClassCounts{Submitted: 800}},
			},
		},
		Netem: &NetemResult{LossEvidence: &NetemLossEvidence{Delta: &netops.QdiscPairDelta{
			ClientEgress: &netops.QdiscCounterDelta{SentBytes: 120_000, SentPackets: 1300},
			ServerEgress: &netops.QdiscCounterDelta{SentBytes: 1_000_000, SentPackets: 900},
		}}},
	}
}

func TestComputeCostAggregatesWindowedSamples(t *testing.T) {
	cost := ComputeCost(costTestResult())
	if cost == nil {
		t.Fatal("cost is nil")
	}
	if cost.WindowNS != 10_000_000_000 || cost.TotalConns != 4 {
		t.Fatalf("window/conns = %+v", cost)
	}
	if cost.Server == nil || cost.Server.CPUTimeNS != 2_000_000_000 ||
		math.Abs(cost.Server.CPUUtilization-0.2) > 1e-9 ||
		cost.Server.MaxRSSBytes != 12<<20 || cost.Server.Processes != 1 {
		t.Fatalf("server cost = %+v", cost.Server)
	}
	if cost.Clients == nil || cost.Clients.CPUTimeNS != 1_500_000_000 ||
		cost.Clients.Processes != 2 || cost.Clients.MaxRSSBytes != 11<<20 {
		t.Fatalf("clients cost = %+v", cost.Clients)
	}
	if cost.DeliveredUnique != 2000 || math.Abs(cost.ServerCPUPerDeliveryNS-1_000_000) > 1e-9 {
		t.Fatalf("useful ops = %+v", cost)
	}
}

func TestComputeCostWireAmplification(t *testing.T) {
	cost := ComputeCost(costTestResult())
	wire := cost.Wire
	if wire == nil {
		t.Fatal("wire cost is nil")
	}
	// client egress app = 1200 x 64 B = 76800 B / server egress app = 800 x 1000 B
	if wire.AppClientEgressBytes != 76_800 || wire.AppServerEgressBytes != 800_000 {
		t.Fatalf("app bytes = %+v", wire)
	}
	if math.Abs(wire.ClientByteAmplification-120_000.0/76_800.0) > 1e-9 ||
		math.Abs(wire.ServerByteAmplification-1.25) > 1e-9 {
		t.Fatalf("byte amplification = %+v", wire)
	}
	if math.Abs(wire.ClientPacketsPerMessage-1300.0/1200.0) > 1e-9 ||
		math.Abs(wire.ServerPacketsPerMessage-900.0/800.0) > 1e-9 {
		t.Fatalf("packets per message = %+v", wire)
	}
}

func TestComputeCostRoomRelayFanout(t *testing.T) {
	result := costTestResult()
	result.Config.Scenario = &ScenarioSpec{
		Name: "room-cost", Kind: ScenarioRoomRelay,
		RoomPublish: &TrafficSpec{
			TrafficID:    3,
			LossTolerant: TrafficClassSpec{RateHz: 20, PayloadBytes: 128, MinDeliveryRatio: 0.9},
		},
	}
	result.Metrics.Traffic = []TrafficAggregate{
		{TrafficID: 3, Direction: DirectionRoomRelay, Class: ClassLossTolerant,
			ClassCounts:      ClassCounts{Submitted: 400},
			ExpectedReceives: 1200},
	}
	wire := ComputeCost(result).Wire
	if wire == nil {
		t.Fatal("wire cost is nil")
	}
	if wire.AppClientEgressBytes != 400*128 || wire.AppServerEgressBytes != 1200*128 {
		t.Fatalf("fanout app bytes = %+v", wire)
	}
}

func TestComputeCostWithoutWindowOrEvidence(t *testing.T) {
	if ComputeCost(&Result{}) != nil {
		t.Fatal("cost without control window should be nil")
	}
	result := costTestResult()
	result.Netem = nil
	cost := ComputeCost(result)
	if cost == nil || cost.Wire != nil {
		t.Fatalf("cost without netem evidence = %+v", cost)
	}
}
