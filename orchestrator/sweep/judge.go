// Package sweep は capacity スライスの二分探索 driver。
// 平面セル(docs/profiles.md)× transport ごとに conns の quality-bounded
// capacity を求める。判定は design spec の「計測意味論の核」に従い
// 物理フロア相対で行う。
package sweep

import (
	"fmt"
	"strings"

	"github.com/neguse/rudp-bench/orchestrator/netops"
	"github.com/neguse/rudp-bench/orchestrator/run"
)

const (
	// deliveryGateFactor: delivery ≥ factor × floor(design spec)
	deliveryGateFactor = 0.95
	// stalenessAllowanceIntervals: staleness p99 ≤ floor + N × 送信間隔
	// (profiles.md の平面 gate 規則: N=1)
	stalenessAllowanceIntervals = 1
	// farmCPUSaturation: client farm を「計算資源律速」とみなす、割当論理 CPU
	// に対する使用率のしきい値。gate 失敗の censoring はこれを超えた場合のみ
	// (ledger #6。CPU が暇なのに送信が遅い = transport client 側の stall で
	// あり、farm のせいにしない — 再生計画 Phase 1-5)
	farmCPUSaturation = 0.85
)

// Judgment は 1 run(1 点)の capacity 判定。
type Judgment struct {
	Outcome  run.Outcome `json:"outcome"`
	OK       bool        `json:"ok"`
	Censored bool        `json:"censored"` // farm 律速 — break ではなく打ち切り
	Cause    string      `json:"cause,omitempty"`

	DeliveryLT    float64                 `json:"delivery_lt,omitempty"`
	DeliveryMD    float64                 `json:"delivery_md,omitempty"`
	StalenessP99  uint64                  `json:"staleness_p99_ns,omitempty"`
	SchedP99      uint64                  `json:"sched_p99_ns,omitempty"` // client 送信スケジュール遅延(farm 診断)
	FloorDelivery float64                 `json:"floor_delivery,omitempty"`
	FloorStaleNS  uint64                  `json:"floor_staleness_ns,omitempty"`
	Traffic       []run.TrafficEvaluation `json:"traffic,omitempty"`
}

// invalid reason の分類。
//   - client受信側drop: farm の受信不足。delivery を過小に見せる
//     計測器起因の欠測なので censored(design spec: farm 律速は打ち切り)
//   - server受信側drop: SUT側の処理飽和であり、run gateがFAILへ分類する
//   - attempted_ratio: 送信 slot の未 submit。TCP 系では transport backpressure
//     そのもの(= 測定対象の挙動)であり、未送信 slot は delivery の分母に
//     入っているので metric gate で正直に判定できる → censored にしない
type reasonKind int

const (
	reasonAttempted reasonKind = iota
	reasonRecvDrop
	reasonOther
)

func classifyReason(reason string) reasonKind {
	switch {
	case strings.Contains(reason, "attempted_ratio"):
		return reasonAttempted
	case strings.Contains(reason, "client netns UDP drop delta"):
		return reasonRecvDrop
	default:
		return reasonOther
	}
}

// deliveryFloorLT は loss-tolerant の物理フロア = Π(1 − link loss)。
// echo / broadcast とも経路は client egress + server egress の 2 リンク。
func deliveryFloorLT(netem *run.NetemRegime) float64 {
	if netem == nil {
		return 1.0
	}
	floor := 1.0
	floor *= 1.0 - netem.ClientEgress.LossPercent/100.0
	floor *= 1.0 - netem.ServerEgress.LossPercent/100.0
	return floor
}

// stalenessFloorNS = 片道遅延(両リンク合算)+ 送信間隔 + サンプル周期
// + バースト黒塗り項。Gilbert-Elliott の loss burst 中はリンク全体が
// 落ちるため、burst長/リンクpps の間は transport 非依存に update が
// 届かない(loss 率 ≥ 1% ならこの黒塗りが p99 サンプルに載る)。
// 一次近似として両方向の黒塗り時間をフロアに加算する。
func stalenessFloorNS(w run.Workload, totalConns int, netem *run.NetemRegime, intervalNS, samplePeriodNS uint64) uint64 {
	var delayNS, burstNS uint64
	if netem != nil {
		delayNS = uint64(netem.ClientEgress.DelayMS+netem.ServerEgress.DelayMS) * 1_000_000
		uplink, downlink := w.LinkPPS(totalConns)
		burstNS += burstGapNS(netem.ClientEgress, uplink)
		burstNS += burstGapNS(netem.ServerEgress, downlink)
	}
	return delayNS + burstNS + intervalNS + samplePeriodNS
}

func burstGapNS(egress netops.Netem, linkPPS float64) uint64 {
	if egress.LossPercent <= 0 || linkPPS <= 0 {
		return 0
	}
	burst := egress.LossBurstLen
	if burst < 1 {
		burst = 1
	}
	return uint64(burst / linkPPS * 1e9)
}

// Judge は run 結果を平面 gate(archetype 非依存)で判定する。
func Judge(result *run.Result, w run.Workload, totalConns int, netem *run.NetemRegime, samplePeriodNS uint64) Judgment {
	j := Judgment{Outcome: result.Outcome}
	if result.Outcome == run.OutcomeFail {
		j.Cause = strings.Join(result.OutcomeReasons, "; ")
		if j.Cause == "" {
			j.Cause = "SUT failed"
		}
		return j
	}

	var causes []string
	backpressureNote := ""
	if result.Verdict != run.VerdictValid {
		hasDrop, hasOther := false, false
		for _, r := range result.InvalidReasons {
			switch classifyReason(r) {
			case reasonRecvDrop:
				hasDrop = true
			case reasonOther:
				hasOther = true
			}
		}
		switch {
		case hasOther || len(result.InvalidReasons) == 0:
			j.Cause = "invalid: " + strings.Join(result.InvalidReasons, "; ")
			return j
		case hasDrop:
			j.Censored = true
			j.Cause = "farm_limited: " + strings.Join(result.InvalidReasons, "; ")
			return j
		default:
			// attempted_ratio のみ → 未 submit slot は delivery の分母に
			// 入っているので metric gate に判定を委ねる。gate も落ちた場合
			// のみ backpressure を原因に併記する(単独では点を落とさない)
			backpressureNote = "submit backpressure (" + strings.Join(result.InvalidReasons, "; ") + ")"
		}
	}
	if result.Metrics == nil {
		j.Cause = "invalid: metrics missing"
		return j
	}

	if w.RateMD > 0 {
		md, ok := result.Metrics.Classes["must_deliver"]
		if !ok {
			causes = append(causes, "must_deliver metrics missing")
		} else {
			j.DeliveryMD = md.DeliveryRatio
			// must-deliver は再送で回復するのでフロア = 1.0
			if md.DeliveryRatio < deliveryGateFactor {
				causes = append(causes, fmt.Sprintf("delivery_md=%.4f below %.2f", md.DeliveryRatio, deliveryGateFactor))
			}
		}
	}

	if w.RateLT > 0 {
		lt, ok := result.Metrics.Classes["loss_tolerant"]
		if !ok {
			causes = append(causes, "loss_tolerant metrics missing")
		} else {
			j.DeliveryLT = lt.DeliveryRatio
			j.FloorDelivery = deliveryFloorLT(netem)
			if lt.DeliveryRatio < deliveryGateFactor*j.FloorDelivery {
				causes = append(causes, fmt.Sprintf("delivery_lt=%.4f below %.2f×floor(%.4f)", lt.DeliveryRatio, deliveryGateFactor, j.FloorDelivery))
			}
		}

		intervalNS := uint64(1e9 / w.RateLT)
		j.FloorStaleNS = stalenessFloorNS(w, totalConns, netem, intervalNS, samplePeriodNS)
		budget := j.FloorStaleNS + stalenessAllowanceIntervals*intervalNS

		if lt, ok := result.Metrics.Classes["loss_tolerant"]; ok {
			j.SchedP99 = lt.LatencySchedNS.P99NS
		}

		st := result.Metrics.StalenessNS
		j.StalenessP99 = st.P99NS
		if st.Count == 0 {
			causes = append(causes, "staleness histogram empty")
		} else if st.P99NS > budget {
			causes = append(causes, fmt.Sprintf("staleness_p99=%dms over floor(%dms)+%d interval", st.P99NS/1_000_000, j.FloorStaleNS/1_000_000, stalenessAllowanceIntervals))
		}

		// farm 十分性の帰属フィルタ(**失敗の帰属**であり単独 gate ではない)。
		// 旧実装は sched 起点 latency p99(= end-to-end。loss 下では transport
		// の HoL/CC がそのまま載る)を「client の送信遅延」とみなして censoring
		// しており、loss regime で transport 側 stall が farm 律速に誤帰属されて
		// いた(ブロック間で censored ≥N と正直 0 に割れる `!` の主因)。
		// v2: client farm の CPU 実測で分離する —
		//   - CPU 飽和 → farm の計算資源律速。censored(rig の限界)
		//   - CPU が暇なのに sched latency が予算超過 → transport client 側の
		//     stall(= その transport を選んだときの実挙動)。censoring せず
		//     正直に break、原因に client_stall を開示
		if len(causes) > 0 && !result.Config.SchedIsMeasurand {
			util, known := clientCPUUtilization(result)
			switch {
			case known && util >= farmCPUSaturation:
				j.Censored = true
				j.Cause = fmt.Sprintf("farm_limited: client farm cpu=%.0f%% of allotted CPUs (saturated); %s",
					util*100, strings.Join(causes, "; "))
				return j
			case j.SchedP99 > budget:
				note := fmt.Sprintf("client_stall: sched p99=%dms over staleness budget %dms with idle farm",
					j.SchedP99/1_000_000, budget/1_000_000)
				if known {
					note += fmt.Sprintf(" (cpu=%.0f%%)", util*100)
				} else {
					note += " (cpu unknown)"
				}
				causes = append(causes, note)
			}
		}
	}

	if len(causes) > 0 {
		if backpressureNote != "" {
			causes = append(causes, backpressureNote)
		}
		j.Cause = strings.Join(causes, "; ")
		return j
	}
	j.OK = true
	return j
}

// JudgeScenario evaluates the absolute SLOs embedded in a v3 scenario. Unlike
// the legacy workload judge it never relaxes an application SLO relative to a
// network floor; an infeasible network/scenario pair fails explicitly.
func JudgeScenario(result *run.Result, scenario run.ScenarioSpec) Judgment {
	j := Judgment{Outcome: result.Outcome}
	switch result.Outcome {
	case run.OutcomeUnsupported, run.OutcomeInconclusive:
		j.Cause = strings.Join(result.OutcomeReasons, "; ")
		return j
	case run.OutcomeFail:
		evaluationIsSLOFailure := result.ScenarioEvaluation != nil && !result.ScenarioEvaluation.OK &&
			len(result.OutcomeReasons) == 1 && result.OutcomeReasons[0] == result.ScenarioEvaluation.Cause
		if !evaluationIsSLOFailure {
			j.Cause = strings.Join(result.OutcomeReasons, "; ")
			if j.Cause == "" {
				j.Cause = "SUT semantic or process failure"
			}
			if result.ScenarioEvaluation != nil {
				j.Traffic = result.ScenarioEvaluation.Traffic
			}
			return j
		}
	}
	if result.Verdict != run.VerdictValid {
		hasDrop, hasOther := false, false
		for _, reason := range result.InvalidReasons {
			switch classifyReason(reason) {
			case reasonRecvDrop:
				hasDrop = true
			case reasonOther:
				hasOther = true
			}
		}
		if hasDrop && !hasOther {
			j.Outcome = run.OutcomeCensored
			j.Censored = true
			j.Cause = "farm_limited: " + strings.Join(result.InvalidReasons, "; ")
			return j
		}
		// attempted-only validity failures are represented in the delivery
		// denominator and can proceed to the SLO evaluation.
		if hasOther || len(result.InvalidReasons) == 0 {
			j.Outcome = run.OutcomeInvalid
			j.Cause = "invalid: " + strings.Join(result.InvalidReasons, "; ")
			return j
		}
	}
	if result.Metrics == nil {
		j.Outcome = run.OutcomeInvalid
		j.Cause = "invalid: metrics missing"
		return j
	}
	evaluation := run.EvaluateScenarioMetrics(result.Metrics, scenario)
	j.OK = evaluation.OK
	j.Cause = evaluation.Cause
	j.Traffic = evaluation.Traffic
	if !j.OK && !result.Config.SchedIsMeasurand {
		if util, known := clientCPUUtilization(result); known && util >= farmCPUSaturation {
			j.Outcome = run.OutcomeCensored
			j.Censored = true
			j.Cause = fmt.Sprintf("farm_limited: client farm cpu=%.0f%% of allotted CPUs (saturated); %s", util*100, j.Cause)
		}
	}
	if j.OK {
		j.Outcome = run.OutcomePass
	} else if j.Outcome == "" || j.Outcome == run.OutcomePass {
		j.Outcome = run.OutcomeFail
	}
	return j
}

// clientCPUUtilization は計測窓内の client プロセス群の CPU 使用率を、
// 割当論理 CPU 数に対する割合で返す(known=false はサンプル不足)。
func clientCPUUtilization(result *run.Result) (float64, bool) {
	if result.Control == nil || len(result.Samples) == 0 {
		return 0, false
	}
	windowNS := result.Control.Schedule.StopAtNS - result.Control.Schedule.StartAtNS
	if windowNS <= 0 {
		return 0, false
	}
	clientPIDs := map[int]bool{}
	for _, p := range result.Processes {
		if p.Role == "client" {
			clientPIDs[p.PID] = true
		}
	}
	if len(clientPIDs) == 0 {
		return 0, false
	}
	var busyNS int64
	seen := false
	for _, series := range result.Samples {
		if !clientPIDs[series.PID] || len(series.Samples) < 2 {
			continue
		}
		first := series.Samples[0]
		last := series.Samples[len(series.Samples)-1]
		if span := last.TimeNS - first.TimeNS; span > 0 {
			// サンプル区間を窓全体に外挿(窓端の欠けを補正)
			busyNS += int64(float64(last.CPUTimeNS-first.CPUTimeNS) / float64(span) * float64(windowNS))
			seen = true
		}
	}
	if !seen {
		return 0, false
	}
	nCPUs := countCPUList(result.Config.ClientCPUs)
	if nCPUs == 0 {
		return 0, false
	}
	return float64(busyNS) / (float64(windowNS) * float64(nCPUs)), true
}

// countCPUList は taskset -c 形式("3,4,11-14" 等)の CPU 数を数える。
func countCPUList(list string) int {
	if list == "" {
		return 0
	}
	n := 0
	for _, part := range strings.Split(list, ",") {
		part = strings.TrimSpace(part)
		if part == "" {
			continue
		}
		if lo, hi, ok := strings.Cut(part, "-"); ok {
			var a, b int
			if _, err := fmt.Sscanf(lo, "%d", &a); err != nil {
				continue
			}
			if _, err := fmt.Sscanf(hi, "%d", &b); err != nil {
				continue
			}
			if b >= a {
				n += b - a + 1
			}
		} else {
			n++
		}
	}
	return n
}
