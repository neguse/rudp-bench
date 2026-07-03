package sweep

import (
	"strings"
	"testing"

	"github.com/neguse/rudp-bench/orchestrator/netops"
	"github.com/neguse/rudp-bench/orchestrator/run"
)

func lossWorstNetem() *run.NetemRegime {
	return &run.NetemRegime{
		ClientEgress: netops.Netem{DelayMS: 25, LossPercent: 3, LossBurstLen: 16},
		ServerEgress: netops.Netem{DelayMS: 25, LossPercent: 3, LossBurstLen: 16},
	}
}

func validResult(deliveryLT, deliveryMD float64, staleP99NS uint64) *run.Result {
	return &run.Result{
		Verdict: run.VerdictValid,
		Metrics: &run.MergedMetrics{
			Classes: map[string]run.ClassAggregate{
				"loss_tolerant": {DeliveryRatio: deliveryLT},
				"must_deliver":  {DeliveryRatio: deliveryMD},
			},
			StalenessNS: run.Histogram{Count: 1000, P99NS: staleP99NS},
		},
	}
}

const samplePeriodNS = 10_000_000 // 10ms

func TestJudgeFloorRelativeOK(t *testing.T) {
	w, _ := run.LookupWorkload("r20p128")
	// 3%×2 リンクの delivery floor = 0.97² ≈ 0.9409。0.94 はフロア達成圏
	// staleness floor = 50ms(delay) + 50ms(interval) + 10ms = 110ms、予算 +50ms = 160ms
	res := validResult(0.94, 1.0, 155_000_000)
	j := Judge(res, w, 1000, lossWorstNetem(), samplePeriodNS)
	if !j.OK {
		t.Fatalf("expected OK at floor: %+v", j)
	}
	if j.FloorDelivery < 0.9408 || j.FloorDelivery > 0.9410 {
		t.Fatalf("floor_delivery=%v, want ≈0.9409", j.FloorDelivery)
	}
	// c1000 では burst 黒塗り項(16/21000pps ≈ 0.8ms)はほぼ消える
	if j.FloorStaleNS < 110_000_000 || j.FloorStaleNS > 112_000_000 {
		t.Fatalf("floor_staleness=%d, want ≈110-112ms", j.FloorStaleNS)
	}
}

func TestJudgeBurstBlackoutFloor(t *testing.T) {
	// 低 conns の loss 平面ではバースト黒塗りが transport 非依存に p99 を支配する。
	// c4 r20p128: uplink 84pps → gap 190ms、downlink 336pps → gap 48ms
	// floor ≈ 50 + 190 + 48 + 50 + 10 ≈ 348ms、予算 ≈ 398ms
	w, _ := run.LookupWorkload("r20p128")
	res := validResult(0.94, 1.0, 344_000_000) // enet 実測相当
	j := Judge(res, w, 4, lossWorstNetem(), samplePeriodNS)
	if !j.OK {
		t.Fatalf("expected OK at blackout floor: %+v", j)
	}
	if j.FloorStaleNS < 340_000_000 || j.FloorStaleNS > 356_000_000 {
		t.Fatalf("floor_staleness=%d, want ≈348ms", j.FloorStaleNS)
	}

	// TCP 系の崩壊(7s)は黒塗りでは説明できず、正しく break になる
	res = validResult(0.99, 1.0, 7_000_000_000)
	j = Judge(res, w, 4, lossWorstNetem(), samplePeriodNS)
	if j.OK || !strings.Contains(j.Cause, "staleness_p99") {
		t.Fatalf("expected staleness break for TCP collapse: %+v", j)
	}
}

func TestJudgeDeliveryBelowFloor(t *testing.T) {
	w, _ := run.LookupWorkload("r20p128")
	// 0.95×floor ≈ 0.8939 を下回る
	res := validResult(0.85, 1.0, 100_000_000)
	j := Judge(res, w, 1000, lossWorstNetem(), samplePeriodNS)
	if j.OK || !strings.Contains(j.Cause, "delivery_lt") {
		t.Fatalf("expected delivery_lt cause: %+v", j)
	}
}

func TestJudgeStalenessOverBudget(t *testing.T) {
	w, _ := run.LookupWorkload("r20p128")
	res := validResult(0.99, 1.0, 200_000_000) // 予算 160ms 超
	j := Judge(res, w, 1000, lossWorstNetem(), samplePeriodNS)
	if j.OK || !strings.Contains(j.Cause, "staleness_p99") {
		t.Fatalf("expected staleness cause: %+v", j)
	}
}

func TestJudgeSelfScalingBudget(t *testing.T) {
	// r60 は interval 16.7ms — 同じ 200ms でも r20 より厳しい予算になる
	w, _ := run.LookupWorkload("r60p200")
	// floor = 50 + 16.7 + 10 = 76.7ms、予算 ≈ 93.3ms
	res := validResult(0.99, 1.0, 95_000_000)
	j := Judge(res, w, 1000, lossWorstNetem(), samplePeriodNS)
	if j.OK {
		t.Fatalf("expected staleness fail at 95ms for r60: %+v", j)
	}
	res = validResult(0.99, 1.0, 90_000_000)
	j = Judge(res, w, 1000, lossWorstNetem(), samplePeriodNS)
	if !j.OK {
		t.Fatalf("expected OK at 90ms for r60: %+v", j)
	}
}

func TestJudgeRecvDropCensored(t *testing.T) {
	w, _ := run.LookupWorkload("r20p128")
	res := &run.Result{
		Verdict:        run.VerdictInvalid,
		InvalidReasons: []string{"client netns UDP drop delta non-zero: InErrors=361 RcvbufErrors=361"},
	}
	j := Judge(res, w, 4, nil, samplePeriodNS)
	if !j.Censored || j.OK {
		t.Fatalf("expected censored: %+v", j)
	}
}

func TestJudgeBackpressureIsBreakNotCensor(t *testing.T) {
	// TCP 系の submit backpressure: attempted 低下は metric gate で正直に落ちる
	w, _ := run.LookupWorkload("r20p128")
	res := validResult(0.81, 0.82, 70_000_000)
	res.Verdict = run.VerdictInvalid
	res.InvalidReasons = []string{"attempted_ratio=0.8137 below threshold=0.99"}
	j := Judge(res, w, 4, nil, samplePeriodNS)
	if j.Censored || j.OK {
		t.Fatalf("expected honest break: %+v", j)
	}
	if !strings.Contains(j.Cause, "delivery_lt") || !strings.Contains(j.Cause, "submit backpressure") {
		t.Fatalf("cause should name gate and note backpressure: %q", j.Cause)
	}
}

func TestJudgeAttemptedShortfallAloneDoesNotFail(t *testing.T) {
	// attempted 0.97 でも quality gate が通れば OK(分母には既に入っている)
	w, _ := run.LookupWorkload("r20p128")
	res := validResult(0.97, 0.98, 70_000_000)
	res.Verdict = run.VerdictInvalid
	res.InvalidReasons = []string{"attempted_ratio=0.97 below threshold=0.99"}
	j := Judge(res, w, 4, nil, samplePeriodNS)
	if !j.OK {
		t.Fatalf("expected OK when gates pass: %+v", j)
	}
}

func TestJudgeOtherInvalidNotCensored(t *testing.T) {
	w, _ := run.LookupWorkload("r20p128")
	res := &run.Result{
		Verdict:        run.VerdictInvalid,
		InvalidReasons: []string{"server proc_index=0 pid=1 exit_code=1"},
	}
	j := Judge(res, w, 4, nil, samplePeriodNS)
	if j.Censored || j.OK || !strings.Contains(j.Cause, "invalid:") {
		t.Fatalf("expected plain invalid: %+v", j)
	}
}

func TestJudgeReliableEchoSkipsLTGates(t *testing.T) {
	w, _ := run.LookupWorkload("reliable_echo")
	res := &run.Result{
		Verdict: run.VerdictValid,
		Metrics: &run.MergedMetrics{
			Classes: map[string]run.ClassAggregate{
				"must_deliver": {DeliveryRatio: 1.0},
			},
			// lt なし → staleness 空でも OK
			StalenessNS: run.Histogram{Count: 0},
		},
	}
	j := Judge(res, w, 4, nil, samplePeriodNS)
	if !j.OK {
		t.Fatalf("expected OK for reliable_echo: %+v", j)
	}
}

func TestJudgeNoNetemFloors(t *testing.T) {
	w, _ := run.LookupWorkload("r20p128")
	res := validResult(0.999, 1.0, 70_000_000)
	j := Judge(res, w, 4, nil, samplePeriodNS)
	if !j.OK {
		t.Fatalf("expected OK without netem: %+v", j)
	}
	if j.FloorDelivery != 1.0 {
		t.Fatalf("floor should be 1.0 without netem: %v", j.FloorDelivery)
	}
	// floor = 0 + 50ms + 10ms = 60ms
	if j.FloorStaleNS != 60_000_000 {
		t.Fatalf("floor_staleness=%d, want 60ms", j.FloorStaleNS)
	}
}
