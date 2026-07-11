package conformance

import (
	"math"
	"testing"
)

func TestConfigValidate(t *testing.T) {
	valid := DefaultConfig(1)
	if reasons := valid.Validate(); len(reasons) != 0 {
		t.Fatalf("valid config rejected: %v", reasons)
	}
	for name, mutate := range map[string]func(*Config){
		"version":           func(c *Config) { c.Version++ },
		"alpha zero":        func(c *Config) { c.FamilyAlpha = 0 },
		"alpha underflow":   func(c *Config) { c.FamilyAlpha = math.SmallestNonzeroFloat64 },
		"alpha one":         func(c *Config) { c.FamilyAlpha = 1 },
		"alpha NaN":         func(c *Config) { c.FamilyAlpha = math.NaN() },
		"alpha infinity":    func(c *Config) { c.FamilyAlpha = math.Inf(1) },
		"alpha scope":       func(c *Config) { c.FamilyAlphaScope = "global" },
		"loss zero":         func(c *Config) { c.LossPercent = 0 },
		"loss one hundred":  func(c *Config) { c.LossPercent = 100 },
		"loss NaN":          func(c *Config) { c.LossPercent = math.NaN() },
		"loss infinity":     func(c *Config) { c.LossPercent = math.Inf(-1) },
		"payload too small": func(c *Config) { c.PayloadBytes = c.LinkMTUBytes / 2 },
		"payload too large": func(c *Config) { c.PayloadBytes = c.LinkMTUBytes },
		"link MTU too large": func(c *Config) {
			c.LinkMTUBytes = maxLinkMTUBytes + 1
			c.PayloadBytes = c.LinkMTUBytes - 1
		},
		"payload does not fit int": func(c *Config) {
			c.LinkMTUBytes = ^uint64(0)
			c.PayloadBytes = maxIntValue + 1
			c.RateHz = 1
			c.WarmupNS = 1
			c.DurationNS = nanosecondsPerSecond
			c.DrainNS = 1
			c.SlotsPerRun = 1
		},
		"slots zero":     func(c *Config) { c.SlotsPerRun = 0 },
		"slots mismatch": func(c *Config) { c.SlotsPerRun++ },
		"payload byte overflow": func(c *Config) {
			c.RateHz = nanosecondsPerSecond
			c.WarmupNS = 1
			c.DurationNS = maxDurationNS - 2
			c.DrainNS = 1
			c.SlotsPerRun = c.DurationNS
		},
		"valid acquisitions zero":    func(c *Config) { c.ValidAcquisitionsPerCase = 0 },
		"max attempts zero":          func(c *Config) { c.MaxAttemptsPerCase = 0 },
		"acquisitions over attempts": func(c *Config) { c.ValidAcquisitionsPerCase = c.MaxAttemptsPerCase + 1 },
		"rate zero":                  func(c *Config) { c.RateHz = 0 },
		"rate non-divisor":           func(c *Config) { c.RateHz = 3 },
		"rate too large":             func(c *Config) { c.RateHz = nanosecondsPerSecond + 1 },
		"warmup zero":                func(c *Config) { c.WarmupNS = 0 },
		"duration zero":              func(c *Config) { c.DurationNS = 0 },
		"drain zero":                 func(c *Config) { c.DrainNS = 0 },
		"warmup overflow":            func(c *Config) { c.WarmupNS = maxDurationNS + 1 },
		"duration overflow":          func(c *Config) { c.DurationNS = maxDurationNS + 1 },
		"drain overflow":             func(c *Config) { c.DrainNS = maxDurationNS + 1 },
		"total duration overflow":    func(c *Config) { c.WarmupNS = maxDurationNS / 2; c.DurationNS = maxDurationNS / 2; c.DrainNS = 2 },
		"fractional slot":            func(c *Config) { c.DurationNS++ },
		"multiple conns":             func(c *Config) { c.TotalConnections = 2 },
		"inexact submit":             func(c *Config) { c.RequireExactSubmission = false },
	} {
		t.Run(name, func(t *testing.T) {
			cfg := valid
			mutate(&cfg)
			if reasons := cfg.Validate(); len(reasons) == 0 {
				t.Fatal("invalid config accepted")
			}
		})
	}
}

func TestDefaultConfigFixesAcquisitionAndTimingPlan(t *testing.T) {
	config := DefaultConfig(1)
	if config.FamilyAlphaScope != FamilyAlphaScopePerTransport ||
		config.ValidAcquisitionsPerCase != 1 || config.MaxAttemptsPerCase != 2 ||
		config.RateHz != 50 || config.WarmupNS != 25*nanosecondsPerSecond ||
		config.DurationNS != 20*nanosecondsPerSecond || config.DrainNS != 5*nanosecondsPerSecond ||
		config.SlotsPerRun != 1000 {
		t.Fatalf("unexpected fixed plan: %+v", config)
	}
}

func TestCanonicalConfigRejectsOtherwiseValidVariant(t *testing.T) {
	config := DefaultConfig(2)
	if reasons := config.Validate(); len(reasons) != 0 {
		t.Fatalf("research variant should remain evaluable: %v", reasons)
	}
	if reasons := config.ValidateCanonical(); len(reasons) == 0 {
		t.Fatal("non-canonical loss treatment was accepted for a conformance session")
	}
	if reasons := DefaultConfig(1).ValidateCanonical(); len(reasons) != 0 {
		t.Fatalf("frozen preset rejected: %v", reasons)
	}
}

func TestRequiredCaseMatrix(t *testing.T) {
	cases := RequiredCases()
	if len(cases) != 6 {
		t.Fatalf("required cases=%d, want 6", len(cases))
	}
	clean, clientLoss, serverLoss := 0, 0, 0
	for _, definition := range cases {
		switch definition.Egress {
		case EgressNone:
			clean++
		case EgressClient:
			clientLoss++
		case EgressServer:
			serverLoss++
		default:
			t.Fatalf("unexpected egress in %+v", definition)
		}
	}
	if clean != 2 || clientLoss != 2 || serverLoss != 2 {
		t.Fatalf("matrix clean/client/server=%d/%d/%d, want 2/2/2", clean, clientLoss, serverLoss)
	}
}
