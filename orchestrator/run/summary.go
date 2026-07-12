package run

import (
	"fmt"
	"os"
	"sort"
)

func writeSummary(path string, result *Result) error {
	f, err := os.Create(path)
	if err != nil {
		return fmt.Errorf("create summary: %w", err)
	}
	defer f.Close()

	fmt.Fprintf(f, "rudp-bench run summary\n")
	fmt.Fprintf(f, "transport: %s\n", result.Transport)
	fmt.Fprintf(f, "outcome: %s\n", result.Outcome)
	for _, reason := range result.OutcomeReasons {
		fmt.Fprintf(f, "- outcome reason: %s\n", reason)
	}
	fmt.Fprintf(f, "verdict: %s\n", result.Verdict)
	if len(result.InvalidReasons) > 0 {
		fmt.Fprintf(f, "invalid reasons:\n")
		for _, reason := range result.InvalidReasons {
			fmt.Fprintf(f, "- %s\n", reason)
		}
	}
	if result.Control != nil {
		fmt.Fprintf(f, "schedule_ns: start=%d stop=%d drain_until=%d\n",
			result.Control.Schedule.StartAtNS,
			result.Control.Schedule.StopAtNS,
			result.Control.Schedule.DrainUntilNS)
		if result.Control.SteadyEnabled {
			fmt.Fprintf(f, "steady_warmup: reached=%t warmup_actual_ms=%d\n",
				result.Control.SteadyReached,
				result.Control.WarmupActualNS/1000000)
		}
	}
	fmt.Fprintf(f, "processes:\n")
	for _, p := range result.Processes {
		fmt.Fprintf(f, "- role=%s proc_index=%d pid=%d exit_code=%d metrics=%s\n",
			p.Role, p.ProcIndex, p.PID, p.ExitCode, p.MetricsOut)
	}
	if result.Metrics != nil {
		fmt.Fprintf(f, "metrics:\n")
		names := make([]string, 0, len(result.Metrics.Classes))
		for name := range result.Metrics.Classes {
			names = append(names, name)
		}
		sort.Strings(names)
		for _, name := range names {
			c := result.Metrics.Classes[name]
			fmt.Fprintf(f, "- class=%s slots=%d submitted=%d attempted=%.6f expected=%d delivered_unique=%d delivery=%.6f duplicates=%d deadline_hit=%d p50_sched_ns=%d p99_sched_ns=%d\n",
				name,
				c.Slots,
				c.Submitted,
				c.AttemptedRatio,
				c.ExpectedReceives,
				c.DeliveredUnique,
				c.DeliveryRatio,
				c.Duplicates,
				c.DeadlineHit,
				c.LatencySchedNS.P50NS,
				c.LatencySchedNS.P99NS)
		}
		fmt.Fprintf(f, "staleness_ns: count=%d p50=%d p90=%d p99=%d\n",
			result.Metrics.StalenessNS.Count,
			result.Metrics.StalenessNS.P50NS,
			result.Metrics.StalenessNS.P90NS,
			result.Metrics.StalenessNS.P99NS)
		for _, name := range metricClassNames {
			g := result.Metrics.Classes[name].UpdateGapNS
			if g.Count > 0 {
				fmt.Fprintf(f, "update_gap_ns[%s]: count=%d p50=%d p90=%d p99=%d\n",
					name, g.Count, g.P50NS, g.P90NS, g.P99NS)
			}
		}
	}
	if result.ScenarioEvaluation != nil {
		fmt.Fprintf(f, "scenario_slo: ok=%v cause=%s\n", result.ScenarioEvaluation.OK, result.ScenarioEvaluation.Cause)
		for _, traffic := range result.ScenarioEvaluation.Traffic {
			fmt.Fprintf(f, "- traffic=%s/%s/%s ok=%v delivery=%.6f deadline_hit=%.6f staleness_p99_ns=%d never_received_flows=%d cause=%s\n",
				traffic.Name, traffic.Direction, traffic.Class, traffic.OK,
				traffic.DeliveryRatio, traffic.DeadlineHitRatio, traffic.StalenessP99NS,
				traffic.NeverReceivedFlows, traffic.Cause)
		}
	}
	if ramp := result.Ramp; ramp != nil {
		fmt.Fprintf(f, "ramp: score_conns=%d censored=%t cause=%s\n",
			ramp.ScoreConns, ramp.Censored, ramp.Cause)
		for _, point := range ramp.Timeline {
			fmt.Fprintf(f, "- ramp index=%d active_conns=%d ok=%t cause=%s\n",
				point.Index, point.ActiveConns, point.Evaluation.OK, point.Evaluation.Cause)
		}
	}
	if cost := result.Cost; cost != nil {
		for _, role := range []*ProcessCost{cost.Server, cost.Clients} {
			if role == nil {
				continue
			}
			fmt.Fprintf(f, "cost[%s]: procs=%d cpu_ns=%d cpu_util=%.4f max_rss_bytes=%d\n",
				role.Role, role.Processes, role.CPUTimeNS, role.CPUUtilization, role.MaxRSSBytes)
		}
		fmt.Fprintf(f, "cost: delivered_unique=%d server_cpu_per_delivery_ns=%.1f conns=%d\n",
			cost.DeliveredUnique, cost.ServerCPUPerDeliveryNS, cost.TotalConns)
		if wire := cost.Wire; wire != nil {
			fmt.Fprintf(f, "wire[client_egress]: sent_bytes=%d sent_packets=%d app_bytes=%d app_messages=%d byte_amp=%.3f pkt_per_msg=%.3f\n",
				wire.ClientEgressSentBytes, wire.ClientEgressSentPackets,
				wire.AppClientEgressBytes, wire.AppClientEgressMessages,
				wire.ClientByteAmplification, wire.ClientPacketsPerMessage)
			fmt.Fprintf(f, "wire[server_egress]: sent_bytes=%d sent_packets=%d app_bytes=%d app_messages=%d byte_amp=%.3f pkt_per_msg=%.3f\n",
				wire.ServerEgressSentBytes, wire.ServerEgressSentPackets,
				wire.AppServerEgressBytes, wire.AppServerEgressMessages,
				wire.ServerByteAmplification, wire.ServerPacketsPerMessage)
		}
	}
	if result.Netem != nil && result.Netem.Enabled {
		fmt.Fprintf(f, "client_udp_drop_delta: InErrors=%d RcvbufErrors=%d\n",
			result.Netem.UDPDelta.InErrors,
			result.Netem.UDPDelta.RcvbufErrors)
		fmt.Fprintf(f, "server_udp_drop_delta: InErrors=%d RcvbufErrors=%d\n",
			result.Netem.ServerUDPDelta.InErrors,
			result.Netem.ServerUDPDelta.RcvbufErrors)
	}
	return nil
}
