package conformance

import (
	"fmt"
	"math"

	"github.com/neguse/rudp-bench/orchestrator/run"
)

const (
	ConfigVersion = 2
	ReportVersion = 2
	GateVersion   = "class-mapping-delivery-v2"

	FamilyAlphaScopePerTransport = "per_transport_four_loss_cases"
	nanosecondsPerSecond         = uint64(1_000_000_000)
	maxDurationNS                = uint64(^uint64(0) >> 1)
	maxIntValue                  = uint64(^uint(0) >> 1)
	maxLinkMTUBytes              = uint64(65535)
)

type ProbeCaseID string

const (
	CaseCleanLossTolerant      ProbeCaseID = "clean_loss_tolerant"
	CaseCleanMustDeliver       ProbeCaseID = "clean_must_deliver"
	CaseLossTolerantClientLoss ProbeCaseID = "loss_tolerant_client_egress_loss"
	CaseLossTolerantServerLoss ProbeCaseID = "loss_tolerant_server_egress_loss"
	CaseMustDeliverClientLoss  ProbeCaseID = "must_deliver_client_egress_loss"
	CaseMustDeliverServerLoss  ProbeCaseID = "must_deliver_server_egress_loss"
)

type Endpoint string

const (
	EndpointBoth   Endpoint = "both"
	EndpointClient Endpoint = "client"
	EndpointServer Endpoint = "server"
)

type Egress string

const (
	EgressNone   Egress = "none"
	EgressClient Egress = "client_egress"
	EgressServer Egress = "server_egress"
)

type CaseDefinition struct {
	ID       ProbeCaseID `json:"id"`
	Class    string      `json:"class"`
	Endpoint Endpoint    `json:"endpoint"`
	Egress   Egress      `json:"loss_egress"`
	Clean    bool        `json:"clean"`
}

var requiredCases = []CaseDefinition{
	{ID: CaseCleanLossTolerant, Class: run.ClassLossTolerant, Endpoint: EndpointBoth, Egress: EgressNone, Clean: true},
	{ID: CaseCleanMustDeliver, Class: run.ClassMustDeliver, Endpoint: EndpointBoth, Egress: EgressNone, Clean: true},
	{ID: CaseLossTolerantClientLoss, Class: run.ClassLossTolerant, Endpoint: EndpointClient, Egress: EgressClient},
	{ID: CaseLossTolerantServerLoss, Class: run.ClassLossTolerant, Endpoint: EndpointServer, Egress: EgressServer},
	{ID: CaseMustDeliverClientLoss, Class: run.ClassMustDeliver, Endpoint: EndpointClient, Egress: EgressClient},
	{ID: CaseMustDeliverServerLoss, Class: run.ClassMustDeliver, Endpoint: EndpointServer, Egress: EgressServer},
}

func RequiredCases() []CaseDefinition {
	out := make([]CaseDefinition, len(requiredCases))
	copy(out, requiredCases)
	return out
}

func caseDefinition(id ProbeCaseID) (CaseDefinition, bool) {
	for _, definition := range requiredCases {
		if definition.ID == id {
			return definition, true
		}
	}
	return CaseDefinition{}, false
}

type Config struct {
	Version                  int     `json:"version"`
	FamilyAlpha              float64 `json:"family_alpha"`
	FamilyAlphaScope         string  `json:"family_alpha_scope"`
	LossPercent              float64 `json:"loss_percent"`
	PayloadBytes             uint64  `json:"payload_bytes"`
	LinkMTUBytes             uint64  `json:"link_mtu_bytes"`
	SlotsPerRun              uint64  `json:"slots_per_run"`
	TotalConnections         int     `json:"total_connections"`
	RequireExactSubmission   bool    `json:"require_exact_submission"`
	ValidAcquisitionsPerCase int     `json:"valid_acquisitions_per_case"`
	MaxAttemptsPerCase       int     `json:"max_attempts_per_case"`
	RateHz                   uint64  `json:"rate_hz"`
	WarmupNS                 uint64  `json:"warmup_ns"`
	DurationNS               uint64  `json:"duration_ns"`
	DrainNS                  uint64  `json:"drain_ns"`
}

func DefaultConfig(lossPercent float64) Config {
	return Config{
		Version:                  ConfigVersion,
		FamilyAlpha:              0.01,
		FamilyAlphaScope:         FamilyAlphaScopePerTransport,
		LossPercent:              lossPercent,
		PayloadBytes:             1000,
		LinkMTUBytes:             1500,
		SlotsPerRun:              1000,
		TotalConnections:         1,
		RequireExactSubmission:   true,
		ValidAcquisitionsPerCase: 1,
		MaxAttemptsPerCase:       2,
		RateHz:                   50,
		WarmupNS:                 25 * nanosecondsPerSecond,
		DurationNS:               20 * nanosecondsPerSecond,
		DrainNS:                  5 * nanosecondsPerSecond,
	}
}

func (c Config) CaseAlpha() float64 {
	return c.FamilyAlpha / 4
}

func (c Config) Validate() []string {
	var reasons []string
	if c.Version != ConfigVersion {
		reasons = append(reasons, fmt.Sprintf("config version=%d, want %d", c.Version, ConfigVersion))
	}
	if math.IsNaN(c.FamilyAlpha) || math.IsInf(c.FamilyAlpha, 0) || c.FamilyAlpha <= 0 || c.FamilyAlpha >= 1 {
		reasons = append(reasons, fmt.Sprintf("family_alpha=%g must be finite and between 0 and 1", c.FamilyAlpha))
	} else if c.CaseAlpha() == 0 {
		reasons = append(reasons, "family_alpha/4 underflows to zero")
	}
	if c.FamilyAlphaScope != FamilyAlphaScopePerTransport {
		reasons = append(reasons, fmt.Sprintf("family_alpha_scope=%q, want %q", c.FamilyAlphaScope, FamilyAlphaScopePerTransport))
	}
	if math.IsNaN(c.LossPercent) || math.IsInf(c.LossPercent, 0) || c.LossPercent <= 0 || c.LossPercent >= 100 {
		reasons = append(reasons, fmt.Sprintf("loss_percent=%g must be finite and between 0 and 100", c.LossPercent))
	}
	if c.LinkMTUBytes == 0 || c.LinkMTUBytes > maxLinkMTUBytes {
		reasons = append(reasons, fmt.Sprintf("link_mtu_bytes=%d must be between 1 and %d", c.LinkMTUBytes, maxLinkMTUBytes))
	}
	if c.PayloadBytes == 0 || c.PayloadBytes >= c.LinkMTUBytes || c.PayloadBytes <= c.LinkMTUBytes/2 {
		reasons = append(reasons, "payload_bytes must be greater than half the link MTU and smaller than the link MTU")
	}
	if c.PayloadBytes > maxIntValue {
		reasons = append(reasons, "payload_bytes must fit int for the run scenario")
	}
	if c.SlotsPerRun == 0 {
		reasons = append(reasons, "slots_per_run must be positive")
	}
	if c.PayloadBytes != 0 && c.SlotsPerRun > ^uint64(0)/c.PayloadBytes {
		reasons = append(reasons, "slots_per_run * payload_bytes overflows uint64")
	}
	if c.ValidAcquisitionsPerCase <= 0 {
		reasons = append(reasons, "valid_acquisitions_per_case must be positive")
	}
	if c.MaxAttemptsPerCase <= 0 {
		reasons = append(reasons, "max_attempts_per_case must be positive")
	} else if c.ValidAcquisitionsPerCase > c.MaxAttemptsPerCase {
		reasons = append(reasons, "valid_acquisitions_per_case exceeds max_attempts_per_case")
	}
	if c.RateHz == 0 || c.RateHz > nanosecondsPerSecond || nanosecondsPerSecond%c.RateHz != 0 {
		reasons = append(reasons, "rate_hz must be a positive integer divisor of 1e9")
	}
	for name, value := range map[string]uint64{
		"warmup_ns":   c.WarmupNS,
		"duration_ns": c.DurationNS,
		"drain_ns":    c.DrainNS,
	} {
		if value == 0 || value > maxDurationNS {
			reasons = append(reasons, fmt.Sprintf("%s must be positive and fit time.Duration", name))
		}
	}
	if c.WarmupNS <= maxDurationNS && c.DurationNS <= maxDurationNS && c.DrainNS <= maxDurationNS &&
		(c.WarmupNS > maxDurationNS-c.DurationNS || c.WarmupNS+c.DurationNS > maxDurationNS-c.DrainNS) {
		reasons = append(reasons, "warmup_ns + duration_ns + drain_ns must fit time.Duration")
	}
	if c.RateHz > 0 && c.RateHz <= nanosecondsPerSecond && nanosecondsPerSecond%c.RateHz == 0 && c.DurationNS > 0 {
		intervalNS := nanosecondsPerSecond / c.RateHz
		if c.DurationNS%intervalNS != 0 {
			reasons = append(reasons, "duration_ns must contain an integer number of rate_hz intervals")
		} else {
			expectedSlots := c.DurationNS / intervalNS
			if expectedSlots != c.SlotsPerRun {
				reasons = append(reasons, fmt.Sprintf("slots_per_run=%d, want rate_hz*duration=%d", c.SlotsPerRun, expectedSlots))
			}
		}
	}
	if c.TotalConnections != 1 {
		reasons = append(reasons, fmt.Sprintf("total_connections=%d, want 1 for the class-exclusive echo probe", c.TotalConnections))
	}
	if !c.RequireExactSubmission {
		reasons = append(reasons, "class-mapping-delivery-v2 requires exact submission")
	}
	return reasons
}

// ValidateCanonical requires the frozen acquisition preset used for comparable
// class-mapping session reports. Evaluate can still consume other fully
// disclosed Config values for research, but the session driver never labels
// those variants as canonical conformance.
func (c Config) ValidateCanonical() []string {
	reasons := c.Validate()
	if c != DefaultConfig(1) {
		reasons = append(reasons, "probe config differs from the frozen class-mapping conformance preset")
	}
	return reasons
}
