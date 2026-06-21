package result

import (
	"os"
	"path/filepath"
	"testing"
)

func TestMedian(t *testing.T) {
	tests := []struct {
		name string
		vals []float64
		want float64
		ok   bool
	}{
		{"empty", nil, 0, false},
		{"single", []float64{5}, 5, true},
		{"odd", []float64{3, 1, 2}, 2, true},
		{"even", []float64{1, 2, 3, 4}, 2.5, true},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got, ok := median(tc.vals)
			if ok != tc.ok {
				t.Fatalf("median ok = %v, want %v", ok, tc.ok)
			}
			if ok && got != tc.want {
				t.Fatalf("median = %v, want %v", got, tc.want)
			}
		})
	}
}

func TestAggregateValidFilter(t *testing.T) {
	dir := t.TempDir()
	results := filepath.Join(dir, "results.csv")
	out := filepath.Join(dir, "summary.csv")

	// Write 3 result rows: 2 valid, 1 invalid.
	if err := EnsureHeader(results, ResultFields); err != nil {
		t.Fatal(err)
	}
	rows := []map[string]string{
		{
			"run_id": "r1", "scenario_id": "s1", "library": "enet",
			"valid": "1", "invalid_reason": "ok",
			"delivery_ratio": "1.0000",
			"server_cpu_pct": "10.00",
		},
		{
			"run_id": "r2", "scenario_id": "s1", "library": "enet",
			"valid": "1", "invalid_reason": "ok",
			"delivery_ratio": "0.9000",
			"server_cpu_pct": "20.00",
		},
		{
			"run_id": "r3", "scenario_id": "s1", "library": "enet",
			"valid": "0", "invalid_reason": "server_crash",
			"delivery_ratio": "0.0000",
			"server_cpu_pct": "99.00",
		},
	}
	for _, r := range rows {
		if err := AppendRow(results, ResultFields, r); err != nil {
			t.Fatal(err)
		}
	}

	if err := Aggregate(results, "", out, 1); err != nil {
		t.Fatal(err)
	}

	outRows, err := ReadCSVRows(out)
	if err != nil {
		t.Fatal(err)
	}
	if len(outRows) != 1 {
		t.Fatalf("got %d output rows, want 1", len(outRows))
	}
	row := outRows[0]

	if row["n_total"] != "3" {
		t.Errorf("n_total = %q, want 3", row["n_total"])
	}
	if row["n_valid"] != "2" {
		t.Errorf("n_valid = %q, want 2", row["n_valid"])
	}
	if row["valid"] != "1" {
		t.Errorf("valid = %q, want 1", row["valid"])
	}
	// Median of 1.0000 and 0.9000 = 0.9500.
	if row["delivery_ratio_median"] != "0.9500" {
		t.Errorf("delivery_ratio_median = %q, want 0.9500", row["delivery_ratio_median"])
	}
	// Median of 10.00 and 20.00 = 15.00.
	if row["server_cpu_pct_median"] != "15.00" {
		t.Errorf("server_cpu_pct_median = %q, want 15.00", row["server_cpu_pct_median"])
	}
	// The invalid row (99.00) should NOT be included.
	_ = os.Remove(results)
	_ = os.Remove(out)
}
