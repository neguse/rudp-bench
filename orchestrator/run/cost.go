package run

import (
	"github.com/neguse/rudp-bench/orchestrator/sampler"
)

// Cost は RQ4(resource cost)と RQ5(機構の説明)向けの measurement window 内
// 集計。判定には使わず、capacity と併記して開示する(ADR-0002 Cost And
// Overhead)。connection あたりの増分 memory は単一 run では定義できないため、
// ここは per-run の素点(RSS、conns)を出し、増分は block 横断で導出する。

type ProcessCost struct {
	Role      string `json:"role"`
	Processes int    `json:"processes"`
	// measurement window 内の CPU 時間(全 process 合算)
	CPUTimeNS int64 `json:"cpu_time_ns"`
	// CPUTimeNS / window。multi-thread では 1 を超える
	CPUUtilization float64 `json:"cpu_utilization"`
	// window 内の最大 RSS(process ごとの最大値の合算)
	MaxRSSBytes uint64 `json:"max_rss_bytes"`
}

type WireCost struct {
	// measurement window 内の qdisc counter 差分(loss evidence と同一 window)
	ClientEgressSentBytes   uint64 `json:"client_egress_sent_bytes"`
	ClientEgressSentPackets uint64 `json:"client_egress_sent_packets"`
	ServerEgressSentBytes   uint64 `json:"server_egress_sent_bytes"`
	ServerEgressSentPackets uint64 `json:"server_egress_sent_packets"`
	// application payload 側(submitted x payload_bytes、fanout は複製後)
	AppClientEgressBytes    uint64 `json:"app_client_egress_bytes"`
	AppClientEgressMessages uint64 `json:"app_client_egress_messages"`
	AppServerEgressBytes    uint64 `json:"app_server_egress_bytes"`
	AppServerEgressMessages uint64 `json:"app_server_egress_messages"`
	// wire / application(app が 0 のときは 0)
	ClientByteAmplification float64 `json:"client_byte_amplification,omitempty"`
	ServerByteAmplification float64 `json:"server_byte_amplification,omitempty"`
	ClientPacketsPerMessage float64 `json:"client_packets_per_message,omitempty"`
	ServerPacketsPerMessage float64 `json:"server_packets_per_message,omitempty"`
}

type CostSummary struct {
	Version  int          `json:"version"`
	WindowNS int64        `json:"window_ns"`
	Server   *ProcessCost `json:"server,omitempty"`
	Clients  *ProcessCost `json:"clients,omitempty"`
	// useful application operation = window 内 delivered_unique(全 class)
	DeliveredUnique        uint64  `json:"delivered_unique"`
	ServerCPUPerDeliveryNS float64 `json:"server_cpu_per_delivery_ns,omitempty"`
	TotalConns             int     `json:"total_conns"`
	Wire                   *WireCost `json:"wire,omitempty"`
}

// ComputeCost derives the cost summary from an assembled result. It returns
// nil when the run has no confirmed measurement window.
func ComputeCost(result *Result) *CostSummary {
	if result == nil || result.Control == nil {
		return nil
	}
	windowNS := result.Control.Schedule.StopAtNS - result.Control.Schedule.StartAtNS
	if windowNS <= 0 {
		return nil
	}
	cost := &CostSummary{Version: 1, WindowNS: windowNS, TotalConns: result.Config.TotalConns}

	roleByPID := map[int]string{}
	for _, process := range result.Processes {
		roleByPID[process.PID] = process.Role
	}
	byRole := map[string]*ProcessCost{}
	for _, series := range result.Samples {
		role, ok := roleByPID[series.PID]
		if !ok || len(series.Samples) < 2 {
			continue
		}
		entry := byRole[role]
		if entry == nil {
			entry = &ProcessCost{Role: role}
			byRole[role] = entry
		}
		entry.Processes++
		entry.CPUTimeNS += seriesCPUDelta(series)
		entry.MaxRSSBytes += seriesMaxRSS(series)
	}
	for _, entry := range byRole {
		entry.CPUUtilization = float64(entry.CPUTimeNS) / float64(windowNS)
	}
	cost.Server = byRole["server"]
	cost.Clients = byRole["client"]

	if metrics := result.Metrics; metrics != nil {
		for _, class := range metrics.Classes {
			cost.DeliveredUnique += class.DeliveredUnique
		}
		if cost.Server != nil && cost.DeliveredUnique > 0 {
			cost.ServerCPUPerDeliveryNS = float64(cost.Server.CPUTimeNS) / float64(cost.DeliveredUnique)
		}
		cost.Wire = computeWireCost(result, metrics)
	}
	return cost
}

func computeWireCost(result *Result, metrics *MergedMetrics) *WireCost {
	if result.Netem == nil || result.Netem.LossEvidence == nil || result.Netem.LossEvidence.Delta == nil {
		return nil
	}
	delta := result.Netem.LossEvidence.Delta
	if delta.ClientEgress == nil || delta.ServerEgress == nil {
		return nil
	}
	scenario := result.Config.Scenario
	if scenario == nil {
		return nil
	}
	payloadByTraffic := map[TrafficKey]uint64{}
	for _, trafficCase := range scenarioMetricCases(*scenario) {
		if trafficCase.spec == nil {
			continue
		}
		id := trafficCase.spec.TrafficID
		if lt := trafficCase.spec.LossTolerant; lt.RateHz > 0 {
			payloadByTraffic[TrafficKey{id, trafficCase.direction, ClassLossTolerant}] = uint64(lt.PayloadBytes)
		}
		if md := trafficCase.spec.MustDeliver; md.RateHz > 0 {
			payloadByTraffic[TrafficKey{id, trafficCase.direction, ClassMustDeliver}] = uint64(md.PayloadBytes)
		}
	}
	wire := &WireCost{
		ClientEgressSentBytes:   delta.ClientEgress.SentBytes,
		ClientEgressSentPackets: delta.ClientEgress.SentPackets,
		ServerEgressSentBytes:   delta.ServerEgress.SentBytes,
		ServerEgressSentPackets: delta.ServerEgress.SentPackets,
	}
	for _, traffic := range metrics.Traffic {
		payload := payloadByTraffic[TrafficKey{traffic.TrafficID, traffic.Direction, traffic.Class}]
		switch traffic.Direction {
		case DirectionClientToServer:
			wire.AppClientEgressMessages += traffic.Submitted
			wire.AppClientEgressBytes += traffic.Submitted * payload
		case DirectionServerToClient:
			wire.AppServerEgressMessages += traffic.Submitted
			wire.AppServerEgressBytes += traffic.Submitted * payload
		case DirectionRoomRelay:
			wire.AppClientEgressMessages += traffic.Submitted
			wire.AppClientEgressBytes += traffic.Submitted * payload
			wire.AppServerEgressMessages += traffic.ExpectedReceives
			wire.AppServerEgressBytes += traffic.ExpectedReceives * payload
		}
	}
	if wire.AppClientEgressBytes > 0 {
		wire.ClientByteAmplification = float64(wire.ClientEgressSentBytes) / float64(wire.AppClientEgressBytes)
	}
	if wire.AppServerEgressBytes > 0 {
		wire.ServerByteAmplification = float64(wire.ServerEgressSentBytes) / float64(wire.AppServerEgressBytes)
	}
	if wire.AppClientEgressMessages > 0 {
		wire.ClientPacketsPerMessage = float64(wire.ClientEgressSentPackets) / float64(wire.AppClientEgressMessages)
	}
	if wire.AppServerEgressMessages > 0 {
		wire.ServerPacketsPerMessage = float64(wire.ServerEgressSentPackets) / float64(wire.AppServerEgressMessages)
	}
	return wire
}

func seriesCPUDelta(series sampler.Series) int64 {
	first, last := series.Samples[0], series.Samples[len(series.Samples)-1]
	if last.CPUTimeNS < first.CPUTimeNS {
		return 0
	}
	return last.CPUTimeNS - first.CPUTimeNS
}

func seriesMaxRSS(series sampler.Series) uint64 {
	var max uint64
	for _, sample := range series.Samples {
		if sample.RSSBytes > max {
			max = sample.RSSBytes
		}
	}
	return max
}
