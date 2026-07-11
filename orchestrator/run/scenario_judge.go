package run

import (
	"fmt"
	"strings"
)

type TrafficEvaluation struct {
	Name               string  `json:"name"`
	TrafficID          uint8   `json:"traffic_id"`
	Direction          string  `json:"direction"`
	Class              string  `json:"class"`
	OK                 bool    `json:"ok"`
	Cause              string  `json:"cause,omitempty"`
	DeliveryRatio      float64 `json:"delivery_ratio"`
	DeadlineHitRatio   float64 `json:"deadline_hit_ratio"`
	ResolvedDeadlineNS uint64  `json:"resolved_deadline_ns,omitempty"`
	StalenessP99NS     uint64  `json:"staleness_p99_ns,omitempty"`
	ExpectedFlows      uint64  `json:"expected_flows,omitempty"`
	NeverReceivedFlows uint64  `json:"never_received_flows,omitempty"`
}

type ScenarioEvaluation struct {
	OK                  bool                `json:"ok"`
	CompletePrimarySLOs bool                `json:"complete_primary_slos"`
	MissingPrimarySLOs  []string            `json:"missing_primary_slos,omitempty"`
	Cause               string              `json:"cause,omitempty"`
	Traffic             []TrafficEvaluation `json:"traffic"`
}

// EvaluateScenarioMetrics applies only the scenario's absolute application
// SLOs. Environment validity and farm censoring remain separate decisions.
func EvaluateScenarioMetrics(metrics *MergedMetrics, scenario ScenarioSpec) ScenarioEvaluation {
	evaluation := ScenarioEvaluation{
		CompletePrimarySLOs: scenario.HasCompletePrimarySLOs(),
		MissingPrimarySLOs:  scenario.MissingPrimarySLOs(),
	}
	if metrics == nil {
		evaluation.Cause = "metrics missing"
		return evaluation
	}
	type trafficCase struct {
		name      string
		direction string
		spec      *TrafficSpec
	}
	var cases []trafficCase
	switch scenario.Kind {
	case ScenarioEnvironmentBaseline:
		cases = append(cases, trafficCase{"baseline", DirectionRoomRelay, scenario.ClientInput})
	case ScenarioAuthoritativeState:
		cases = append(cases,
			trafficCase{"client_input", DirectionClientToServer, scenario.ClientInput},
			trafficCase{"server_state", DirectionServerToClient, scenario.ServerState})
	case ScenarioRoomRelay:
		cases = append(cases, trafficCase{"room_publish", DirectionRoomRelay, scenario.RoomPublish})
	default:
		evaluation.Cause = fmt.Sprintf("unknown scenario kind %q", scenario.Kind)
		return evaluation
	}

	var causes []string
	for _, tc := range cases {
		if tc.spec == nil {
			continue
		}
		for _, classCase := range []struct {
			name string
			spec TrafficClassSpec
		}{
			{ClassLossTolerant, tc.spec.LossTolerant},
			{ClassMustDeliver, tc.spec.MustDeliver},
		} {
			if classCase.spec.RateHz <= 0 {
				continue
			}
			metric, found := metrics.TrafficMetric(tc.spec.TrafficID, tc.direction, classCase.name)
			traffic := TrafficEvaluation{
				Name: tc.name, TrafficID: tc.spec.TrafficID, Direction: tc.direction,
				Class: classCase.name,
			}
			var trafficCauses []string
			if !found {
				trafficCauses = append(trafficCauses, "metrics missing")
			} else {
				traffic.DeliveryRatio = metric.DeliveryRatio
				traffic.DeadlineHitRatio = metric.DeadlineHitRatio
				traffic.ResolvedDeadlineNS = metric.DeadlineNS
				traffic.StalenessP99NS = metric.StalenessNS.P99NS
				traffic.ExpectedFlows = metric.ExpectedFlows
				traffic.NeverReceivedFlows = metric.NeverReceivedFlows
				if metric.Duplicates != 0 && classCase.name == ClassMustDeliver {
					trafficCauses = append(trafficCauses, fmt.Sprintf("duplicates=%d", metric.Duplicates))
				}
				if metric.DeadlineNS != tc.spec.MustDeliver.DeadlineNS {
					trafficCauses = append(trafficCauses, fmt.Sprintf("resolved_deadline_ns=%d, want %d", metric.DeadlineNS, tc.spec.MustDeliver.DeadlineNS))
				}
				if classCase.spec.MinDeliveryRatio > 0 && metric.DeliveryRatio < classCase.spec.MinDeliveryRatio {
					trafficCauses = append(trafficCauses, fmt.Sprintf("delivery=%.6f below %.6f", metric.DeliveryRatio, classCase.spec.MinDeliveryRatio))
				}
				if classCase.spec.MinEventualDeliveryRatio > 0 && metric.DeliveryRatio < classCase.spec.MinEventualDeliveryRatio {
					trafficCauses = append(trafficCauses, fmt.Sprintf("eventual_delivery=%.6f below %.6f", metric.DeliveryRatio, classCase.spec.MinEventualDeliveryRatio))
				}
				if classCase.spec.MinDeadlineHitRatio > 0 && metric.DeadlineHitRatio < classCase.spec.MinDeadlineHitRatio {
					trafficCauses = append(trafficCauses, fmt.Sprintf("deadline_hit=%.6f below %.6f", metric.DeadlineHitRatio, classCase.spec.MinDeadlineHitRatio))
				}
				if metric.NeverReceivedFlows > 0 {
					trafficCauses = append(trafficCauses, fmt.Sprintf("never_received_flows=%d", metric.NeverReceivedFlows))
				}
				if classCase.spec.StalenessP99NS > 0 {
					if metric.StalenessNS.Count == 0 {
						trafficCauses = append(trafficCauses, "staleness histogram empty")
					} else if metric.StalenessNS.P99NS > classCase.spec.StalenessP99NS {
						trafficCauses = append(trafficCauses, fmt.Sprintf("staleness_p99=%dms over %dms", metric.StalenessNS.P99NS/1_000_000, classCase.spec.StalenessP99NS/1_000_000))
					}
				}
			}
			traffic.OK = len(trafficCauses) == 0
			traffic.Cause = strings.Join(trafficCauses, "; ")
			evaluation.Traffic = append(evaluation.Traffic, traffic)
			if !traffic.OK {
				causes = append(causes, tc.name+"/"+classCase.name+": "+traffic.Cause)
			}
		}
	}
	evaluation.OK = len(causes) == 0
	evaluation.Cause = strings.Join(causes, "; ")
	return evaluation
}
