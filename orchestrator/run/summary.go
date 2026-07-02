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
	}
	if result.Netem != nil && result.Netem.Enabled {
		fmt.Fprintf(f, "client_udp_drop_delta: InErrors=%d RcvbufErrors=%d\n",
			result.Netem.UDPDelta.InErrors,
			result.Netem.UDPDelta.RcvbufErrors)
	}
	return nil
}
