package run

import (
	"fmt"
	"strconv"
	"time"
)

type ScenarioKind string

const (
	ScenarioEnvironmentBaseline ScenarioKind = "environment_baseline"
	ScenarioAuthoritativeState  ScenarioKind = "authoritative_state"
	ScenarioRoomRelay           ScenarioKind = "room_relay"
)

const (
	TrafficIDLegacy uint8 = iota
	TrafficIDClientInput
	TrafficIDServerState
)

// TrafficClassSpec describes one semantic class within an application flow.
// A zero rate disables the class.
type TrafficClassSpec struct {
	RateHz                   float64 `json:"rate_hz"`
	PayloadBytes             int     `json:"payload_bytes"`
	DeadlineNS               uint64  `json:"deadline_ns,omitempty"`
	StalenessP99NS           uint64  `json:"staleness_p99_ns,omitempty"`
	MinDeliveryRatio         float64 `json:"min_delivery_ratio,omitempty"`
	MinDeadlineHitRatio      float64 `json:"min_deadline_hit_ratio,omitempty"`
	MinEventualDeliveryRatio float64 `json:"min_eventual_delivery_ratio,omitempty"`
}

type TrafficSpec struct {
	TrafficID    uint8            `json:"traffic_id"`
	LossTolerant TrafficClassSpec `json:"loss_tolerant"`
	MustDeliver  TrafficClassSpec `json:"must_deliver"`
}

func (t TrafficSpec) rate() float64 {
	return t.LossTolerant.RateHz + t.MustDeliver.RateHz
}

func (t TrafficSpec) validate(name string, minPayload int) error {
	if t.TrafficID == TrafficIDLegacy {
		return fmt.Errorf("%s.traffic_id must be non-zero", name)
	}
	for className, class := range map[string]TrafficClassSpec{
		"loss_tolerant": t.LossTolerant,
		"must_deliver":  t.MustDeliver,
	} {
		if class.RateHz < 0 {
			return fmt.Errorf("%s.%s.rate_hz must be >= 0", name, className)
		}
		if class.RateHz == 0 && class != (TrafficClassSpec{}) {
			return fmt.Errorf("%s.%s is disabled (rate_hz=0) but has non-zero settings", name, className)
		}
		if class.RateHz > 0 && class.PayloadBytes < minPayload {
			return fmt.Errorf("%s.%s.payload_bytes must be >= %d when enabled", name, className, minPayload)
		}
		for field, ratio := range map[string]float64{
			"min_delivery_ratio":          class.MinDeliveryRatio,
			"min_deadline_hit_ratio":      class.MinDeadlineHitRatio,
			"min_eventual_delivery_ratio": class.MinEventualDeliveryRatio,
		} {
			if ratio < 0 || ratio > 1 {
				return fmt.Errorf("%s.%s.%s must be between 0 and 1", name, className, field)
			}
		}
	}
	if t.rate() == 0 {
		return fmt.Errorf("%s must enable at least one traffic class", name)
	}
	if t.LossTolerant.DeadlineNS != 0 || t.LossTolerant.MinDeadlineHitRatio != 0 ||
		t.LossTolerant.MinEventualDeliveryRatio != 0 {
		return fmt.Errorf("%s.loss_tolerant uses staleness/min_delivery, not deadline/eventual-delivery SLOs", name)
	}
	if t.MustDeliver.StalenessP99NS != 0 || t.MustDeliver.MinDeliveryRatio != 0 {
		return fmt.Errorf("%s.must_deliver uses deadline/eventual-delivery, not staleness/min_delivery SLOs", name)
	}
	if t.MustDeliver.MinDeadlineHitRatio > 0 && t.MustDeliver.DeadlineNS == 0 {
		return fmt.Errorf("%s.must_deliver.deadline_ns is required with min_deadline_hit_ratio", name)
	}
	return nil
}

// ScenarioSpec is the normalized, machine-readable scenario contract. Reference
// presets and user-authored scenarios use the same type.
type ScenarioSpec struct {
	Name        string       `json:"name"`
	Kind        ScenarioKind `json:"kind"`
	ClientInput *TrafficSpec `json:"client_input,omitempty"`
	ServerState *TrafficSpec `json:"server_state,omitempty"`
	RoomPublish *TrafficSpec `json:"room_publish,omitempty"`
}

func (s ScenarioSpec) Validate() error {
	if !IsSafeName(s.Name) {
		return fmt.Errorf("scenario.name must be a path-safe ASCII slug")
	}
	switch s.Kind {
	case ScenarioEnvironmentBaseline:
		if s.ClientInput == nil {
			return fmt.Errorf("environment_baseline requires client_input probe traffic")
		}
		if s.ServerState != nil || s.RoomPublish != nil {
			return fmt.Errorf("environment_baseline only accepts client_input")
		}
		return s.ClientInput.validate("scenario.client_input", 32)
	case ScenarioAuthoritativeState:
		if s.ClientInput == nil || s.ServerState == nil {
			return fmt.Errorf("authoritative_state requires client_input and server_state")
		}
		if s.RoomPublish != nil {
			return fmt.Errorf("authoritative_state does not accept room_publish")
		}
		if s.ClientInput.LossTolerant.RateHz <= 0 || s.ServerState.LossTolerant.RateHz <= 0 {
			return fmt.Errorf("authoritative_state requires loss_tolerant client_input and server_state for latest-value semantics")
		}
		if s.ClientInput.TrafficID == s.ServerState.TrafficID {
			return fmt.Errorf("client_input and server_state traffic_id must differ")
		}
		if err := s.ClientInput.validate("scenario.client_input", 32); err != nil {
			return err
		}
		return s.ServerState.validate("scenario.server_state", 40)
	case ScenarioRoomRelay:
		if s.RoomPublish == nil {
			return fmt.Errorf("room_relay requires room_publish")
		}
		if s.ClientInput != nil || s.ServerState != nil {
			return fmt.Errorf("room_relay only accepts room_publish")
		}
		return s.RoomPublish.validate("scenario.room_publish", 32)
	default:
		return fmt.Errorf("unknown scenario kind %q", s.Kind)
	}
}

// MissingPrimarySLOs returns enabled traffic series that have no explicit
// application threshold. Such scenarios are valid diagnostics, but cannot be
// called PASS or used for capacity search.
func (s ScenarioSpec) MissingPrimarySLOs() []string {
	var missing []string
	for _, traffic := range scenarioMetricCases(s) {
		if traffic.spec == nil {
			continue
		}
		lt := traffic.spec.LossTolerant
		if lt.RateHz > 0 && lt.MinDeliveryRatio == 0 && lt.StalenessP99NS == 0 {
			missing = append(missing, traffic.name+"/"+ClassLossTolerant)
		}
		md := traffic.spec.MustDeliver
		if md.RateHz > 0 && md.MinDeadlineHitRatio == 0 && md.MinEventualDeliveryRatio == 0 {
			missing = append(missing, traffic.name+"/"+ClassMustDeliver)
		}
	}
	return missing
}

func (s ScenarioSpec) HasCompletePrimarySLOs() bool {
	return len(s.MissingPrimarySLOs()) == 0
}

func IsSafeName(value string) bool {
	if len(value) == 0 || len(value) > 96 {
		return false
	}
	for i := 0; i < len(value); i++ {
		c := value[i]
		if (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
			(i > 0 && (c == '-' || c == '_' || c == '.')) {
			continue
		}
		return false
	}
	return true
}

func (s ScenarioSpec) CommonArgs(totalConns int) []string {
	args := []string{"--scenario", string(s.Kind), "--total-conns", strconv.Itoa(totalConns)}
	appendTraffic := func(prefix string, t *TrafficSpec) {
		if t == nil {
			return
		}
		args = append(args,
			"--"+prefix+"-traffic-id", strconv.Itoa(int(t.TrafficID)),
			"--"+prefix+"-rate-lt", formatRate(t.LossTolerant.RateHz),
			"--"+prefix+"-rate-md", formatRate(t.MustDeliver.RateHz),
			"--"+prefix+"-payload-lt", strconv.Itoa(t.LossTolerant.PayloadBytes),
			"--"+prefix+"-payload-md", strconv.Itoa(t.MustDeliver.PayloadBytes),
			"--"+prefix+"-deadline-ns", strconv.FormatUint(t.MustDeliver.DeadlineNS, 10),
		)
	}
	appendTraffic("input", s.ClientInput)
	appendTraffic("state", s.ServerState)
	appendTraffic("publish", s.RoomPublish)
	return args
}

// LinkPPS returns offered application messages per second at each emulated
// egress. It intentionally excludes transport retransmission and framing.
func (s ScenarioSpec) LinkPPS(totalConns int) (clientEgress, serverEgress float64) {
	n := float64(totalConns)
	switch s.Kind {
	case ScenarioEnvironmentBaseline:
		if s.ClientInput != nil {
			clientEgress = n * s.ClientInput.rate()
			serverEgress = clientEgress
		}
	case ScenarioAuthoritativeState:
		if s.ClientInput != nil {
			clientEgress = n * s.ClientInput.rate()
		}
		if s.ServerState != nil {
			serverEgress = n * s.ServerState.rate()
		}
	case ScenarioRoomRelay:
		if s.RoomPublish != nil {
			clientEgress = n * s.RoomPublish.rate()
			serverEgress = clientEgress * n
		}
	}
	return clientEgress, serverEgress
}

func autoDurationScenario(s ScenarioSpec, totalConns int, netem *NetemRegime) time.Duration {
	need := autoDurationMin
	if netem == nil {
		return need
	}
	clientPPS, serverPPS := s.LinkPPS(totalConns)
	for _, direction := range []struct {
		lossPct float64
		burst   float64
		pps     float64
	}{
		{netem.ClientEgress.LossPercent, netem.ClientEgress.LossBurstLen, clientPPS},
		{netem.ServerEgress.LossPercent, netem.ServerEgress.LossBurstLen, serverPPS},
	} {
		if d := lossEventDuration(direction.lossPct, direction.burst, direction.pps); d > need {
			need = d
		}
	}
	if need > autoDurationMax {
		return autoDurationMax
	}
	return need
}
