package run

import (
	"context"
	"fmt"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/control"
	"github.com/neguse/rudp-bench/orchestrator/monotonic"
	"github.com/neguse/rudp-bench/orchestrator/netops"
	"github.com/neguse/rudp-bench/orchestrator/netops/losstrace"
)

const (
	lossEvidenceModeRandomNetem        = "random_netem_qdisc"
	lossEvidenceModeDeterministicTrace = "deterministic_losstrace"
	lossEvidenceScopeEffectiveInner    = "effective_measurement_window_inner"
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

// collectDeterministicLossEvidence samples the losstrace packet counters
// strictly inside the effective measurement window. The trace bitmap is
// deterministic, so the counter range identifies exactly which packets the
// program dropped (known-packet accounting): expected drops in the window are
// the popcount of the trace over [before, after).
func collectDeterministicLossEvidence(ctx context.Context, windowCh <-chan control.ScheduleMessage, pair netops.PairSpec, netem *NetemRegime) *NetemLossEvidence {
	evidence := &NetemLossEvidence{
		Version:   1,
		Mode:      lossEvidenceModeDeterministicTrace,
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
	det := &DeterministicLossEvidence{}
	directions := deterministicDirections(det, pair, netem)
	if len(directions) == 0 {
		evidence.Errors = append(evidence.Errors, "no seeded loss direction is configured")
		return evidence
	}
	evidence.Deterministic = det

	if err := waitUntilMonotonic(ctx, evidence.Schedule.StartAtNS); err != nil {
		evidence.Errors = append(evidence.Errors, "wait for measurement start: "+err.Error())
		return evidence
	}
	var err error
	det.CaptureBeforeStartNS, det.CaptureBeforeFinishNS, err = sampleTraceCounters(directions, func(d *DeterministicLossDirection, v uint64) { d.CounterBefore = v })
	if err != nil {
		evidence.Errors = append(evidence.Errors, "read trace counters after measurement start: "+err.Error())
		return evidence
	}

	captureDuration := time.Duration(det.CaptureBeforeFinishNS - det.CaptureBeforeStartNS)
	lead := 4*captureDuration + 10*time.Millisecond
	if lead < 50*time.Millisecond {
		lead = 50 * time.Millisecond
	}
	finalCaptureAt := evidence.Schedule.StopAtNS - lead.Nanoseconds()
	if finalCaptureAt <= det.CaptureBeforeFinishNS {
		evidence.Errors = append(evidence.Errors, fmt.Sprintf(
			"measurement window too short for non-overlapping trace counter samples: before_finish_ns=%d final_capture_target_ns=%d",
			det.CaptureBeforeFinishNS, finalCaptureAt))
	} else if err := waitUntilMonotonic(ctx, finalCaptureAt); err != nil {
		evidence.Errors = append(evidence.Errors, "wait for final in-window trace counter capture: "+err.Error())
		return evidence
	}
	det.CaptureAfterStartNS, det.CaptureAfterFinishNS, err = sampleTraceCounters(directions, func(d *DeterministicLossDirection, v uint64) { d.CounterAfter = v })
	if err != nil {
		evidence.Errors = append(evidence.Errors, "read trace counters before measurement stop: "+err.Error())
		return evidence
	}

	for _, direction := range directions {
		if direction.record.CounterAfter < direction.record.CounterBefore {
			evidence.Errors = append(evidence.Errors, fmt.Sprintf(
				"%s trace counter went backwards: before=%d after=%d",
				direction.record.Dev, direction.record.CounterBefore, direction.record.CounterAfter))
			continue
		}
		direction.record.Packets = direction.record.CounterAfter - direction.record.CounterBefore
		words, realized, err := losstrace.Generate(direction.egress.LossSeed, direction.egress.LossPercent,
			direction.egress.LossBurstLen, direction.record.TraceBits)
		if err != nil {
			evidence.Errors = append(evidence.Errors, fmt.Sprintf("regenerate %s trace: %v", direction.record.Dev, err))
			continue
		}
		direction.record.RealizedLossPct = realized
		direction.record.TraceSHA256 = HashValue(words)
		drops, err := losstrace.CountDropsInRange(words, direction.record.CounterBefore, direction.record.CounterAfter)
		if err != nil {
			evidence.Errors = append(evidence.Errors, fmt.Sprintf("count %s trace drops: %v", direction.record.Dev, err))
			continue
		}
		direction.record.ExpectedDrops = drops
	}
	return evidence
}

type deterministicDirection struct {
	egress netops.Netem
	record *DeterministicLossDirection
}

func deterministicDirections(det *DeterministicLossEvidence, pair netops.PairSpec, netem *NetemRegime) []deterministicDirection {
	var out []deterministicDirection
	if netem == nil {
		return out
	}
	if netem.ServerEgress.LossPercent > 0 && netem.ServerEgress.LossSeed != 0 {
		det.ServerEgress = &DeterministicLossDirection{
			Namespace: pair.ServerNS, Dev: pair.ServerVeth, TraceBits: traceBitsOrDefault(netem.ServerEgress),
		}
		out = append(out, deterministicDirection{netem.ServerEgress, det.ServerEgress})
	}
	if netem.ClientEgress.LossPercent > 0 && netem.ClientEgress.LossSeed != 0 {
		det.ClientEgress = &DeterministicLossDirection{
			Namespace: pair.ClientNS, Dev: pair.ClientVeth, TraceBits: traceBitsOrDefault(netem.ClientEgress),
		}
		out = append(out, deterministicDirection{netem.ClientEgress, det.ClientEgress})
	}
	return out
}

func traceBitsOrDefault(egress netops.Netem) int {
	if egress.TraceBits > 0 {
		return egress.TraceBits
	}
	return losstrace.DefaultBits
}

func sampleTraceCounters(directions []deterministicDirection, assign func(*DeterministicLossDirection, uint64)) (startNS, finishNS int64, err error) {
	startNS, err = monotonic.NowNS()
	if err != nil {
		return 0, 0, err
	}
	for _, direction := range directions {
		value, readErr := netops.ReadLossTraceCounter(direction.record.Namespace, direction.record.Dev)
		if readErr != nil {
			return 0, 0, fmt.Errorf("%s/%s: %w", direction.record.Namespace, direction.record.Dev, readErr)
		}
		assign(direction.record, value)
	}
	finishNS, err = monotonic.NowNS()
	if err != nil {
		return 0, 0, err
	}
	return startNS, finishNS, nil
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
