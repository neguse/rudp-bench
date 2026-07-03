// Package sweep は capacity スライスの二分探索 driver。
// 平面セル(docs/profiles.md)× transport ごとに conns の quality-bounded
// capacity を求める。判定は design spec の「計測意味論の核」に従い
// 物理フロア相対で行う。
package sweep

import (
	"fmt"
	"strings"

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
	FloorDelivery float64 `json:"floor_delivery,omitempty"`
	FloorStaleNS  uint64  `json:"floor_staleness_ns,omitempty"`
}

// farmLimitedReason は client farm の十分性 gate 発火を表す invalid reason か。
// design spec: これらは server の break ではなく censored として扱う。
func farmLimitedReason(reason string) bool {
	return strings.Contains(reason, "attempted_ratio") ||
		strings.Contains(reason, "UDP drop delta")
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

// stalenessFloorNS = 片道遅延(両リンク合算)+ 送信間隔 + サンプル周期。
func stalenessFloorNS(netem *run.NetemRegime, intervalNS, samplePeriodNS uint64) uint64 {
	var delayNS uint64
	if netem != nil {
		delayNS = uint64(netem.ClientEgress.DelayMS+netem.ServerEgress.DelayMS) * 1_000_000
	}
	return delayNS + intervalNS + samplePeriodNS
}

// Judge は run 結果を平面 gate(archetype 非依存)で判定する。
func Judge(result *run.Result, w run.Workload, netem *run.NetemRegime, samplePeriodNS uint64) Judgment {
	j := Judgment{}

	if result.Verdict != run.VerdictValid {
		allFarm := len(result.InvalidReasons) > 0
		for _, r := range result.InvalidReasons {
			if !farmLimitedReason(r) {
				allFarm = false
				break
			}
		}
		if allFarm {
			j.Censored = true
			j.Cause = "farm_limited: " + strings.Join(result.InvalidReasons, "; ")
		} else {
			j.Cause = "invalid: " + strings.Join(result.InvalidReasons, "; ")
		}
		return j
	}
	if result.Metrics == nil {
		j.Cause = "invalid: metrics missing"
		return j
	}

	var causes []string

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
		j.FloorStaleNS = stalenessFloorNS(netem, intervalNS, samplePeriodNS)
		budget := j.FloorStaleNS + stalenessAllowanceIntervals*intervalNS
		st := result.Metrics.StalenessNS
		j.StalenessP99 = st.P99NS
		if st.Count == 0 {
			causes = append(causes, "staleness histogram empty")
		} else if st.P99NS > budget {
			causes = append(causes, fmt.Sprintf("staleness_p99=%dms over floor(%dms)+%d interval", st.P99NS/1_000_000, j.FloorStaleNS/1_000_000, stalenessAllowanceIntervals))
		}
	}

	if len(causes) > 0 {
		j.Cause = strings.Join(causes, "; ")
		return j
	}
	j.OK = true
	return j
}
