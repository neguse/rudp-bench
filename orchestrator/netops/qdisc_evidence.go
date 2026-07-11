package netops

import (
	"context"
	"fmt"
	"os/exec"

	"github.com/neguse/rudp-bench/orchestrator/monotonic"
)

// QdiscSample brackets one kernel qdisc counter read with CLOCK_MONOTONIC.
// Stats is absent when tc execution or parsing failed; Raw remains available
// for diagnosis in the run bundle.
type QdiscSample struct {
	Namespace       string      `json:"namespace"`
	Device          string      `json:"device"`
	CaptureStartNS  int64       `json:"capture_start_ns"`
	CaptureFinishNS int64       `json:"capture_finish_ns"`
	Stats           *QdiscStats `json:"stats,omitempty"`
	Raw             string      `json:"raw,omitempty"`
	Error           string      `json:"error,omitempty"`
}

type QdiscPairSnapshot struct {
	CaptureStartNS  int64       `json:"capture_start_ns"`
	CaptureFinishNS int64       `json:"capture_finish_ns"`
	ServerEgress    QdiscSample `json:"server_egress"`
	ClientEgress    QdiscSample `json:"client_egress"`
}

type QdiscCounterDelta struct {
	SentBytes   uint64 `json:"sent_bytes"`
	SentPackets uint64 `json:"sent_packets"`
	Dropped     uint64 `json:"dropped"`
	Overlimits  uint64 `json:"overlimits"`
	Requeues    uint64 `json:"requeues"`
}

type QdiscPairDelta struct {
	ServerEgress *QdiscCounterDelta `json:"server_egress,omitempty"`
	ClientEgress *QdiscCounterDelta `json:"client_egress,omitempty"`
	Errors       []string           `json:"errors,omitempty"`
}

// ReadQdiscPairSnapshot reads both egress qdiscs separately so each direction
// carries its own command, raw output, parsed counters, and timing bracket.
func ReadQdiscPairSnapshot(ctx context.Context, spec PairSpec) (QdiscPairSnapshot, error) {
	startNS, err := monotonic.NowNS()
	if err != nil {
		return QdiscPairSnapshot{}, fmt.Errorf("qdisc snapshot start clock: %w", err)
	}
	snapshot := QdiscPairSnapshot{CaptureStartNS: startNS}
	snapshot.ServerEgress = readQdiscSample(ctx, spec.ServerNS, spec.ServerVeth)
	snapshot.ClientEgress = readQdiscSample(ctx, spec.ClientNS, spec.ClientVeth)
	finishNS, err := monotonic.NowNS()
	if err != nil {
		return snapshot, fmt.Errorf("qdisc snapshot finish clock: %w", err)
	}
	snapshot.CaptureFinishNS = finishNS
	return snapshot, nil
}

func readQdiscSample(ctx context.Context, namespace, device string) QdiscSample {
	sample := QdiscSample{Namespace: namespace, Device: device}
	startNS, err := monotonic.NowNS()
	if err != nil {
		sample.Error = fmt.Sprintf("capture start clock: %v", err)
		return sample
	}
	sample.CaptureStartNS = startNS
	cmd := exec.CommandContext(ctx, "tc", "-n", namespace, "-s", "qdisc", "show", "dev", device)
	out, cmdErr := cmd.CombinedOutput()
	sample.Raw = string(out)
	finishNS, clockErr := monotonic.NowNS()
	sample.CaptureFinishNS = finishNS
	if clockErr != nil {
		sample.Error = fmt.Sprintf("capture finish clock: %v", clockErr)
		return sample
	}
	if cmdErr != nil {
		sample.Error = fmt.Sprintf("tc qdisc show: %v", cmdErr)
		return sample
	}
	stats, err := ParseQdiscShow(sample.Raw)
	if err != nil {
		sample.Error = err.Error()
		return sample
	}
	root, err := selectRootQdisc(stats)
	if err != nil {
		sample.Error = err.Error()
		return sample
	}
	sample.Stats = &root
	return sample
}

func selectRootQdisc(stats []QdiscStats) (QdiscStats, error) {
	var roots []QdiscStats
	for _, stat := range stats {
		if stat.Root {
			roots = append(roots, stat)
		}
	}
	if len(roots) != 1 {
		return QdiscStats{}, fmt.Errorf("found %d root qdiscs, want 1", len(roots))
	}
	return roots[0], nil
}

// DeltaQdiscPair subtracts cumulative counters without allowing wrap or qdisc
// replacement to masquerade as a valid measurement delta.
func DeltaQdiscPair(before, after QdiscPairSnapshot) QdiscPairDelta {
	var delta QdiscPairDelta
	delta.ServerEgress = deltaQdiscSample("server_egress", before.ServerEgress, after.ServerEgress, &delta.Errors)
	delta.ClientEgress = deltaQdiscSample("client_egress", before.ClientEgress, after.ClientEgress, &delta.Errors)
	return delta
}

func deltaQdiscSample(direction string, before, after QdiscSample, errs *[]string) *QdiscCounterDelta {
	if before.Error != "" {
		*errs = append(*errs, fmt.Sprintf("%s before: %s", direction, before.Error))
	}
	if after.Error != "" {
		*errs = append(*errs, fmt.Sprintf("%s after: %s", direction, after.Error))
	}
	if before.Stats == nil || after.Stats == nil {
		if before.Error == "" && before.Stats == nil {
			*errs = append(*errs, direction+" before stats are missing")
		}
		if after.Error == "" && after.Stats == nil {
			*errs = append(*errs, direction+" after stats are missing")
		}
		return nil
	}
	b, a := *before.Stats, *after.Stats
	if b.Kind != a.Kind {
		*errs = append(*errs, fmt.Sprintf("%s qdisc kind changed: %q -> %q", direction, b.Kind, a.Kind))
		return nil
	}
	if a.SentBytes < b.SentBytes || a.SentPackets < b.SentPackets || a.Dropped < b.Dropped ||
		a.Overlimits < b.Overlimits || a.Requeues < b.Requeues {
		*errs = append(*errs, direction+" qdisc counters regressed")
		return nil
	}
	return &QdiscCounterDelta{
		SentBytes:   a.SentBytes - b.SentBytes,
		SentPackets: a.SentPackets - b.SentPackets,
		Dropped:     a.Dropped - b.Dropped,
		Overlimits:  a.Overlimits - b.Overlimits,
		Requeues:    a.Requeues - b.Requeues,
	}
}
