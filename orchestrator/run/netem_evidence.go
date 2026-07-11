package run

import (
	"context"
	"fmt"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/control"
	"github.com/neguse/rudp-bench/orchestrator/monotonic"
	"github.com/neguse/rudp-bench/orchestrator/netops"
)

const (
	lossEvidenceModeRandomNetem        = "random_netem_qdisc"
	lossEvidenceModeDeterministicTrace = "deterministic_losstrace"
	lossEvidenceScopeEffectiveInner    = "effective_measurement_window_inner"
	lossEvidenceScopeUnsupported       = "unverified"
	qdiscReadTimeout                   = 2 * time.Second
)

func configuredLoss(netem *NetemRegime) bool {
	return netem != nil && (netem.ServerEgress.LossPercent > 0 || netem.ClientEgress.LossPercent > 0)
}

func deterministicLoss(netem *NetemRegime) bool {
	return netem != nil &&
		((netem.ServerEgress.LossPercent > 0 && netem.ServerEgress.LossSeed != 0) ||
			(netem.ClientEgress.LossPercent > 0 && netem.ClientEgress.LossSeed != 0))
}

func unsupportedDeterministicLossEvidence() *NetemLossEvidence {
	return &NetemLossEvidence{
		Version:   1,
		Mode:      lossEvidenceModeDeterministicTrace,
		Supported: false,
		Scope:     lossEvidenceScopeUnsupported,
		Errors: []string{
			"qdisc counters do not observe deterministic losstrace eBPF drops",
		},
	}
}

func collectRandomNetemLossEvidence(ctx context.Context, windowCh <-chan control.ScheduleMessage, pair netops.PairSpec) *NetemLossEvidence {
	evidence := &NetemLossEvidence{
		Version:   1,
		Mode:      lossEvidenceModeRandomNetem,
		Supported: true,
		Scope:     lossEvidenceScopeEffectiveInner,
	}
	select {
	case schedule := <-windowCh:
		evidence.Schedule = schedule
	case <-ctx.Done():
		evidence.Errors = append(evidence.Errors, "effective measurement window unavailable: "+ctx.Err().Error())
		return evidence
	}
	if evidence.Schedule.StartAtNS <= 0 || evidence.Schedule.StopAtNS <= evidence.Schedule.StartAtNS {
		evidence.Errors = append(evidence.Errors, "effective measurement window is invalid")
		return evidence
	}

	if err := waitUntilMonotonic(ctx, evidence.Schedule.StartAtNS); err != nil {
		evidence.Errors = append(evidence.Errors, "wait for measurement start: "+err.Error())
		return evidence
	}
	before, err := readQdiscPairWithTimeout(ctx, pair)
	evidence.Before = &before
	if err != nil {
		evidence.Errors = append(evidence.Errors, "read qdisc counters after measurement start: "+err.Error())
		return evidence
	}

	captureDuration := time.Duration(before.CaptureFinishNS - before.CaptureStartNS)
	lead := 4*captureDuration + 10*time.Millisecond
	if lead < 50*time.Millisecond {
		lead = 50 * time.Millisecond
	}
	finalCaptureAt := evidence.Schedule.StopAtNS - lead.Nanoseconds()
	if finalCaptureAt <= before.CaptureFinishNS {
		evidence.Errors = append(evidence.Errors, fmt.Sprintf(
			"measurement window too short for non-overlapping qdisc samples: before_finish_ns=%d final_capture_target_ns=%d",
			before.CaptureFinishNS, finalCaptureAt))
	} else if err := waitUntilMonotonic(ctx, finalCaptureAt); err != nil {
		evidence.Errors = append(evidence.Errors, "wait for final in-window qdisc capture: "+err.Error())
		return evidence
	}
	after, err := readQdiscPairWithTimeout(ctx, pair)
	evidence.After = &after
	if err != nil {
		evidence.Errors = append(evidence.Errors, "read qdisc counters before measurement stop: "+err.Error())
		return evidence
	}
	delta := netops.DeltaQdiscPair(before, after)
	evidence.Delta = &delta
	return evidence
}

func readQdiscPairWithTimeout(ctx context.Context, pair netops.PairSpec) (netops.QdiscPairSnapshot, error) {
	readCtx, cancel := context.WithTimeout(ctx, qdiscReadTimeout)
	defer cancel()
	return netops.ReadQdiscPairSnapshot(readCtx, pair)
}

func waitUntilMonotonic(ctx context.Context, targetNS int64) error {
	for {
		nowNS, err := monotonic.NowNS()
		if err != nil {
			return err
		}
		if nowNS >= targetNS {
			return nil
		}
		timer := time.NewTimer(time.Duration(targetNS - nowNS))
		select {
		case <-timer.C:
		case <-ctx.Done():
			timer.Stop()
			return ctx.Err()
		}
	}
}
