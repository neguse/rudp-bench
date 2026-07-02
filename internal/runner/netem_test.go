package runner

import (
	"strings"
	"testing"
)

// TestApplyNetemValidation は不正な引数が sudo tc 実行前に明示エラーに
// なることを確認する（正常系は tc/sudo が必要なためここでは扱わない）。
func TestApplyNetemValidation(t *testing.T) {
	tests := []struct {
		name    string
		args    string
		wantSub string
	}{
		{"empty", "", "at least delay_ms"},
		{"non_numeric_delay", "abc", "not a number"},
		{"negative_delay", "-5", "must be >= 0"},
		{"non_numeric_jitter", "10 x", "not a number"},
		{"negative_loss", "10 2 -1", "must be >= 0"},
		{"loss_over_100", "10 2 101", "must be <= 100"},
		{"non_integer_limit", "10 2 1 12.5", "positive integer"},
		{"zero_limit", "10 2 1 0", "positive integer"},
		{"too_many_fields", "10 2 1 100 9", "too many fields"},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			err := ApplyNetem(tc.args, "")
			if err == nil {
				t.Fatalf("ApplyNetem(%q) = nil, want error containing %q", tc.args, tc.wantSub)
			}
			if !strings.Contains(err.Error(), tc.wantSub) {
				t.Fatalf("ApplyNetem(%q) error = %q, want substring %q", tc.args, err.Error(), tc.wantSub)
			}
		})
	}
}
