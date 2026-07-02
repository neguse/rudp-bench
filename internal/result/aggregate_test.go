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

	if err := Aggregate(results, "", out, FixedMinValid(1)); err != nil {
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

func TestDominantTieBreak(t *testing.T) {
	tests := []struct {
		name string
		vals []string
		want string
	}{
		{"empty", nil, ""},
		{"majority", []string{"b", "a", "b"}, "b"},
		// 同数の場合は辞書順で決定的に選ぶ。
		{"tie", []string{"b", "a"}, "a"},
		{"tie_reversed", []string{"a", "b"}, "a"},
		{"tie_three_way", []string{"c", "b", "a"}, "a"},
		{"skip_empty", []string{"", "", "z"}, "z"},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			// map の反復順に依存しないことを確認するため複数回実行する。
			for i := 0; i < 20; i++ {
				if got := dominant(tc.vals); got != tc.want {
					t.Fatalf("dominant(%v) = %q, want %q", tc.vals, got, tc.want)
				}
			}
		})
	}
}

// TestAggregatePerLibMinValid verifies that a per-library MinValidPolicy
// relaxes the threshold only for the intended library (adaptive skip case).
func TestAggregatePerLibMinValid(t *testing.T) {
	dir := t.TempDir()
	results := filepath.Join(dir, "results.csv")
	scenarios := filepath.Join(dir, "scenarios.csv")
	out := filepath.Join(dir, "summary.csv")

	if err := EnsureHeader(results, ResultFields); err != nil {
		t.Fatal(err)
	}
	// enet は adaptive skip で N=1、kcp も N=1 だが skip していない想定。
	rows := []map[string]string{
		{
			"run_id": "p_c1_r1", "scenario_id": "s_enet", "library": "enet",
			"valid": "1", "invalid_reason": "ok", "delivery_ratio": "1.0000",
		},
		{
			"run_id": "p_c1_r1", "scenario_id": "s_kcp", "library": "kcp",
			"valid": "1", "invalid_reason": "ok", "delivery_ratio": "1.0000",
		},
	}
	for _, r := range rows {
		if err := AppendRow(results, ResultFields, r); err != nil {
			t.Fatal(err)
		}
	}

	if err := EnsureHeader(scenarios, ScenarioFields); err != nil {
		t.Fatal(err)
	}
	scenRows := []map[string]string{
		{"run_id": "p_c1_r1", "scenario_id": "s_enet", "library": "enet", "profile": "echo", "conns": "1"},
		{"run_id": "p_c1_r1", "scenario_id": "s_kcp", "library": "kcp", "profile": "echo", "conns": "1"},
	}
	for _, r := range scenRows {
		if err := AppendRow(scenarios, ScenarioFields, r); err != nil {
			t.Fatal(err)
		}
	}

	policy := func(lib, sid string) int {
		if lib == "enet" {
			return 1 // adaptive skip した lib のみ緩和
		}
		return 2
	}
	if err := Aggregate(results, scenarios, out, policy); err != nil {
		t.Fatal(err)
	}

	outRows, err := ReadCSVRows(out)
	if err != nil {
		t.Fatal(err)
	}
	if len(outRows) != 2 {
		t.Fatalf("got %d output rows, want 2", len(outRows))
	}
	byLib := map[string]map[string]string{}
	for _, r := range outRows {
		byLib[r["library"]] = r
	}
	if byLib["enet"]["valid"] != "1" {
		t.Errorf("enet valid = %q, want 1 (minValid=1)", byLib["enet"]["valid"])
	}
	if byLib["kcp"]["valid"] != "0" {
		t.Errorf("kcp valid = %q, want 0 (minValid=2, n_valid=1)", byLib["kcp"]["valid"])
	}
	// scenarios から profile 列が伝播していること。
	if byLib["enet"]["profile"] != "echo" {
		t.Errorf("enet profile = %q, want echo", byLib["enet"]["profile"])
	}
}

func TestRemoveRows(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "results.csv")

	if err := EnsureHeader(path, ResultFields); err != nil {
		t.Fatal(err)
	}
	rows := []map[string]string{
		{"run_id": "p_c1_r1", "scenario_id": "s_enet", "library": "enet", "valid": "1"},
		{"run_id": "p_c1_r1", "scenario_id": "s_kcp", "library": "kcp", "valid": "1"},
		{"run_id": "p_c1_r2", "scenario_id": "s_enet", "library": "enet", "valid": "1"},
	}
	for _, r := range rows {
		if err := AppendRow(path, ResultFields, r); err != nil {
			t.Fatal(err)
		}
	}

	removed, err := RemoveRows(path, func(row map[string]string) bool {
		return row["run_id"] == "p_c1_r1" && row["library"] == "enet"
	})
	if err != nil {
		t.Fatal(err)
	}
	if removed != 1 {
		t.Fatalf("removed = %d, want 1", removed)
	}

	remaining, err := ReadCSVRows(path)
	if err != nil {
		t.Fatal(err)
	}
	if len(remaining) != 2 {
		t.Fatalf("got %d remaining rows, want 2", len(remaining))
	}
	for _, r := range remaining {
		if r["run_id"] == "p_c1_r1" && r["library"] == "enet" {
			t.Errorf("stale row still present: %v", r)
		}
	}

	// 一致行がない場合は書き換えなしで 0 を返す。
	removed, err = RemoveRows(path, func(row map[string]string) bool {
		return row["library"] == "quiche"
	})
	if err != nil {
		t.Fatal(err)
	}
	if removed != 0 {
		t.Fatalf("removed = %d, want 0", removed)
	}

	// 存在しないファイルは no-op。
	removed, err = RemoveRows(filepath.Join(dir, "missing.csv"), func(map[string]string) bool { return true })
	if err != nil || removed != 0 {
		t.Fatalf("missing file: removed=%d err=%v, want 0, nil", removed, err)
	}
}
