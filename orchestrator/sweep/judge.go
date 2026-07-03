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
)

// Judgment は 1 run(1 点)の capacity 判定。
type Judgment struct {
	OK       bool   `json:"ok"`
	Censored bool   `json:"censored"` // farm 律速 — break ではなく打ち切り
	Cause    string `json:"cause,omitempty"`

	DeliveryLT    float64 `json:"delivery_lt,omitempty"`
	DeliveryMD    float64 `json:"delivery_md,omitempty"`
	StalenessP99  uint64  `json:"staleness_p99_ns,omitempty"`
	SchedP99      uint64  `json:"sched_p99_ns,omitempty"` // client 送信スケジュール遅延(farm 診断)
	FloorDelivery float64 `json:"floor_delivery,omitempty"`
	FloorStaleNS  uint64  `json:"floor_staleness_ns,omitempty"`
}

// invalid reason の分類。
// - 受信側 drop(UDP drop delta): farm の受信不足。delivery を過小に見せる
//   計測器起因の欠測なので censored(design spec: farm 律速は打ち切り)
// - attempted_ratio: 送信 slot の未 submit。TCP 系では transport backpressure
//   そのもの(= 測定対象の挙動)であり、未送信 slot は delivery の分母に
//   入っているので metric gate で正直に判定できる → censored にしない
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
	case strings.Contains(reason, "UDP drop delta"):
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
	j := Judgment{}

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

		// farm 十分性: pacing 遅延(design spec の3点セットの1つ、v1 client_tick
		// gate の v2 版)。**失敗の帰属フィルタ**であり単独 gate ではない —
		// end-to-end が予算内なら client の sched 揺れは結果が吸収済み。
		// gate が落ち、かつ client 自身の送信遅延が staleness 予算を超えている
		// 場合のみ「劣化を server に帰属できない」→ censored。
		if len(causes) > 0 && j.SchedP99 > budget {
			j.Censored = true
			j.Cause = fmt.Sprintf("farm_limited: client sched p99=%dms exceeds staleness budget %dms (pacing stall); %s",
				j.SchedP99/1_000_000, budget/1_000_000, strings.Join(causes, "; "))
			return j
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
