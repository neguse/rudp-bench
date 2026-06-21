package result

import "testing"

func TestInvalidReasonUnsupportedReliable(t *testing.T) {
	// raw_udp doesn't support reliable channel.
	opts := AppendOpts{
		Library:      "raw_udp",
		RateR:        "100",
		RateU:        "0",
		Size:         "64",
		Conns:        "1",
		Mode:         "echo",
		Duration:     "2",
		ServerStatus: "0",
		ClientStatus: "0",
	}
	reason := invalidReason(nil, nil, opts)
	if reason != "unsupported_reliable" {
		t.Errorf("invalidReason = %q, want unsupported_reliable", reason)
	}
}

func TestInvalidReasonTimeout(t *testing.T) {
	opts := AppendOpts{
		Library:      "raw_udp",
		RateR:        "0",
		RateU:        "100",
		Size:         "64",
		Conns:        "1",
		Mode:         "echo",
		Duration:     "2",
		ServerStatus: "124", // timeout
		ClientStatus: "0",
	}
	reason := invalidReason(nil, nil, opts)
	if reason != "server_timeout" {
		t.Errorf("invalidReason = %q, want server_timeout", reason)
	}
}

func TestInvalidReasonOK(t *testing.T) {
	server := map[string]string{
		"server_received":         "200",
		"server_echo_accepted":    "200",
		"server_received_r":       "0",
		"server_received_u":       "200",
		"server_echo_accepted_r":  "0",
		"server_echo_accepted_u":  "200",
		"cpu_pct":                 "7.50",
	}
	client := map[string]string{
		"delivered":               "200",
		"accepted":               "200",
		"client_tick_ok":          "1",
		"client_attempted":       "200",
		"client_accepted":        "200",
		"client_attempted_ratio":  "1.0000",
		"client_accepted_ratio":   "1.0000",
	}
	opts := AppendOpts{
		Library:      "raw_udp",
		RateR:        "0",
		RateU:        "100",
		Size:         "64",
		Conns:        "1",
		Mode:         "echo",
		Duration:     "2",
		ServerStatus: "0",
		ClientStatus: "0",
	}
	reason := invalidReason(server, client, opts)
	if reason != "ok" {
		t.Errorf("invalidReason = %q, want ok", reason)
	}
}

func TestCanonicalDeliveryRatio(t *testing.T) {
	client := map[string]string{
		"delivered": "950",
		"accepted":  "1000",
	}
	got := CanonicalDeliveryRatio(client)
	if got != "0.9500" {
		t.Errorf("canonicalDeliveryRatio = %q, want 0.9500", got)
	}
}

func TestCanonicalDeliveryRatioNil(t *testing.T) {
	got := CanonicalDeliveryRatio(nil)
	if got != "" {
		t.Errorf("canonicalDeliveryRatio(nil) = %q, want empty", got)
	}
}

func TestRecomputedTickOK(t *testing.T) {
	tests := []struct {
		name string
		raw  map[string]string
		want string
	}{
		{
			name: "nil",
			raw:  nil,
			want: "",
		},
		{
			name: "all_good",
			raw: map[string]string{
				"rate_r":                 "0",
				"rate_u":                 "100",
				"client_accepted_ratio":  "1.0000",
				"client_attempted_ratio": "1.0000",
			},
			want: "1",
		},
		{
			name: "low_accepted",
			raw: map[string]string{
				"rate_r":                 "0",
				"rate_u":                 "100",
				"client_accepted_ratio":  "0.9800",
				"client_attempted_ratio": "1.0000",
			},
			want: "0",
		},
		{
			name: "no_rate_only_accepted_matters",
			raw: map[string]string{
				"rate_r":                 "0",
				"rate_u":                 "0",
				"client_accepted_ratio":  "0.9900",
				"client_attempted_ratio": "0.5000",
			},
			want: "1",
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got := RecomputedTickOK(tc.raw)
			if got != tc.want {
				t.Errorf("RecomputedTickOK = %q, want %q", got, tc.want)
			}
		})
	}
}
