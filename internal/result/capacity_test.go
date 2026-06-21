package result

import (
	"testing"

	"github.com/neguse/rudp-bench/internal/bench"
)

func testProfile() bench.Profile {
	return bench.Profile{
		Name:  "test_profile",
		Mode:  "echo",
		RateR: 50,
		RateU: 50,
		Size:  64,
		Conns: []int{1, 50, 200},
	}
}

func TestUpdateCapacityBroken(t *testing.T) {
	tracker := NewCapacityTracker()
	profile := testProfile()

	// First point OK.
	summaryOK := map[string]map[string]string{
		"enet": {
			"valid":                  "1",
			"delivery_ratio_median":  "1.0000",
			"server_cpu_pct_median":  "10.00",
		},
	}
	broken := tracker.UpdateRows(profile, []string{"enet"}, summaryOK, 50, 0.95)
	if len(broken) != 0 {
		t.Fatalf("expected no broken libs at conns=50, got %v", broken)
	}

	// Second point: delivery below gate.
	summaryBad := map[string]map[string]string{
		"enet": {
			"valid":                  "1",
			"delivery_ratio_median":  "0.8000",
			"server_cpu_pct_median":  "90.00",
		},
	}
	broken = tracker.UpdateRows(profile, []string{"enet"}, summaryBad, 200, 0.95)
	if len(broken) != 1 || broken[0] != "enet" {
		t.Fatalf("expected [enet] broken, got %v", broken)
	}

	cap := tracker.rows[[2]string{profile.Name, "enet"}]
	if cap.Status != "broken" {
		t.Errorf("status = %q, want broken", cap.Status)
	}
	if cap.LastOKConns != "50" {
		t.Errorf("last_ok_conns = %q, want 50", cap.LastOKConns)
	}
	if cap.BreakConns != "200" {
		t.Errorf("break_conns = %q, want 200", cap.BreakConns)
	}
	if cap.BreakDelivery != "0.8000" {
		t.Errorf("break_delivery = %q, want 0.8000", cap.BreakDelivery)
	}
}

func TestUpdateCapacityUnsupported(t *testing.T) {
	tracker := NewCapacityTracker()
	profile := testProfile()
	profile.RateR = 50
	profile.RateU = 0

	// raw_udp doesn't support reliable — unsupported_reliable.
	summary := map[string]map[string]string{
		"raw_udp": {
			"valid": "0",
			"note":  "unsupported_reliable",
		},
	}
	broken := tracker.UpdateRows(profile, []string{"raw_udp"}, summary, 1, 0.95)
	if len(broken) != 1 || broken[0] != "raw_udp" {
		t.Fatalf("expected [raw_udp] broken, got %v", broken)
	}

	cap := tracker.rows[[2]string{profile.Name, "raw_udp"}]
	if cap.Status != "unsupported" {
		t.Errorf("status = %q, want unsupported", cap.Status)
	}
	if cap.LastOKConns != "unsupported" {
		t.Errorf("last_ok_conns = %q, want unsupported", cap.LastOKConns)
	}
}

func TestBelowGate(t *testing.T) {
	tracker := NewCapacityTracker()
	profile := testProfile()

	// First point fails delivery < min_delivery.
	summary := map[string]map[string]string{
		"enet": {
			"valid":                  "1",
			"delivery_ratio_median":  "0.5000",
			"server_cpu_pct_median":  "5.00",
		},
	}
	broken := tracker.UpdateRows(profile, []string{"enet"}, summary, 1, 0.95)
	if len(broken) != 1 {
		t.Fatalf("expected 1 broken lib, got %d", len(broken))
	}

	cap := tracker.rows[[2]string{profile.Name, "enet"}]
	if cap.Status != "below_gate" {
		t.Errorf("status = %q, want below_gate", cap.Status)
	}
	if cap.LastOKConns != "below_gate" {
		t.Errorf("last_ok_conns = %q, want below_gate", cap.LastOKConns)
	}
	if cap.BreakConns != "1" {
		t.Errorf("break_conns = %q, want 1", cap.BreakConns)
	}
	if cap.BreakDelivery != "0.5000" {
		t.Errorf("break_delivery = %q, want 0.5000", cap.BreakDelivery)
	}
}
