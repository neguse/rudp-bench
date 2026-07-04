package aggregate

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/neguse/rudp-bench/orchestrator/sweep"
)

// writeCapacityBlock は 1 ブロック分の capacity.json を t.TempDir() 配下に書く。
func writeCapacityBlock(t *testing.T, cells []sweep.CellRecord) string {
	t.Helper()
	dir := t.TempDir()
	doc := struct {
		Seed  int64              `json:"seed"`
		Cells []sweep.CellRecord `json:"cells"`
	}{Seed: 1, Cells: cells}
	data, err := json.Marshal(doc)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "capacity.json"), data, 0o644); err != nil {
		t.Fatal(err)
	}
	return dir
}

func cell(transport, workload, regime string, capacity int, censored, rangeLimited bool) sweep.CellRecord {
	return sweep.CellRecord{
		Transport: transport,
		Workload:  workload,
		Regime:    regime,
		CellCapacity: sweep.CellCapacity{
			Capacity:     capacity,
			Censored:     censored,
			RangeLimited: rangeLimited,
		},
	}
}

// 3 ブロックとも honest break: median/IQR が期待どおりに計算されること。
func TestAggregateCapacityHonest(t *testing.T) {
	dirs := []string{
		writeCapacityBlock(t, []sweep.CellRecord{cell("enet", "echo", "wired", 10, false, false)}),
		writeCapacityBlock(t, []sweep.CellRecord{cell("enet", "echo", "wired", 20, false, false)}),
		writeCapacityBlock(t, []sweep.CellRecord{cell("enet", "echo", "wired", 30, false, false)}),
	}
	aggs, err := AggregateCapacity(dirs)
	if err != nil {
		t.Fatal(err)
	}
	a, ok := aggs[CapacityKey{Transport: "enet", Workload: "echo", Regime: "wired"}]
	if !ok {
		t.Fatal("cell not found")
	}
	if a.N != 3 || a.CensoredN != 0 {
		t.Fatalf("N=%d CensoredN=%d, want 3/0", a.N, a.CensoredN)
	}
	if a.Median != 20 {
		t.Fatalf("Median=%v, want 20", a.Median)
	}
	if a.IQLo != 15 || a.IQHi != 25 {
		t.Fatalf("IQLo=%v IQHi=%v, want 15/25", a.IQLo, a.IQHi)
	}
	if a.Conflicted {
		t.Fatal("should not be conflicted")
	}
	if a.LowerBound != 0 {
		t.Fatalf("LowerBound=%v, want 0 (no censored blocks)", a.LowerBound)
	}
	// N=3 honest → CI が計算されているはず(常に非退化とは限らないが、
	// 全て honest なら少なくとも lo<=hi は保証される)。
	if a.CILo > a.CIHi {
		t.Fatalf("CILo=%v > CIHi=%v", a.CILo, a.CIHi)
	}
	if a.CILo == 0 && a.CIHi == 0 {
		t.Fatal("CI should be populated for N=3 honest values")
	}
}

// 全ブロック censored: 集約は下限の主張のみで、値は max(v_i)。
func TestAggregateCapacityCensoredOnlyIsMaxLowerBound(t *testing.T) {
	dirs := []string{
		writeCapacityBlock(t, []sweep.CellRecord{cell("gns", "r60p200", "wired", 5, true, false)}),
		writeCapacityBlock(t, []sweep.CellRecord{cell("gns", "r60p200", "wired", 8, true, false)}),
		writeCapacityBlock(t, []sweep.CellRecord{cell("gns", "r60p200", "wired", 6, false, true)}), // range_limited も下限扱い
	}
	aggs, err := AggregateCapacity(dirs)
	if err != nil {
		t.Fatal(err)
	}
	a := aggs[CapacityKey{Transport: "gns", Workload: "r60p200", Regime: "wired"}]
	if a.N != 3 || a.CensoredN != 3 {
		t.Fatalf("N=%d CensoredN=%d, want 3/3", a.N, a.CensoredN)
	}
	if a.LowerBound != 8 {
		t.Fatalf("LowerBound=%v, want max(5,8,6)=8", a.LowerBound)
	}
	if a.Median != 0 || a.IQLo != 0 || a.IQHi != 0 {
		t.Fatalf("no honest values → Median/IQR should stay 0, got %v/%v/%v", a.Median, a.IQLo, a.IQHi)
	}
	if a.Conflicted {
		t.Fatal("pure lower-bound case is not 'conflicted' (no honest baseline to conflict with)")
	}
	if a.CILo != 0 || a.CIHi != 0 {
		t.Fatalf("no honest values → CI should stay 0, got %v/%v", a.CILo, a.CIHi)
	}
}

// mixed: censored 下限が honest 最大値以下 → 無視され、honest ベースの中央値が報告される。
func TestAggregateCapacityMixedNoConflict(t *testing.T) {
	dirs := []string{
		writeCapacityBlock(t, []sweep.CellRecord{cell("litenetlib", "r20p128", "wired", 10, false, false)}),
		writeCapacityBlock(t, []sweep.CellRecord{cell("litenetlib", "r20p128", "wired", 12, false, false)}),
		writeCapacityBlock(t, []sweep.CellRecord{cell("litenetlib", "r20p128", "wired", 11, false, false)}),
		writeCapacityBlock(t, []sweep.CellRecord{cell("litenetlib", "r20p128", "wired", 5, true, false)}), // 下限5 <= honest max(12) → 無視
	}
	aggs, err := AggregateCapacity(dirs)
	if err != nil {
		t.Fatal(err)
	}
	a := aggs[CapacityKey{Transport: "litenetlib", Workload: "r20p128", Regime: "wired"}]
	if a.N != 4 || a.CensoredN != 1 {
		t.Fatalf("N=%d CensoredN=%d, want 4/1", a.N, a.CensoredN)
	}
	if a.Median != 11 {
		t.Fatalf("Median=%v, want 11 (honest median of 10,11,12)", a.Median)
	}
	if a.Conflicted {
		t.Fatal("censored bound below honest max should not be conflicted")
	}
	if a.MaxLowerBound != 0 {
		t.Fatalf("MaxLowerBound=%v, want 0 (not set when not conflicted)", a.MaxLowerBound)
	}
}

// mixed: censored 下限が honest 最大値を上回る → Conflicted=true、honest 中央値はそのまま報告。
func TestAggregateCapacityMixedConflict(t *testing.T) {
	dirs := []string{
		writeCapacityBlock(t, []sweep.CellRecord{cell("msquic", "r10p1000", "wired", 10, false, false)}),
		writeCapacityBlock(t, []sweep.CellRecord{cell("msquic", "r10p1000", "wired", 12, false, false)}),
		writeCapacityBlock(t, []sweep.CellRecord{cell("msquic", "r10p1000", "wired", 11, false, false)}),
		writeCapacityBlock(t, []sweep.CellRecord{cell("msquic", "r10p1000", "wired", 20, true, false)}), // 下限20 > honest max(12) → 食い違い
	}
	aggs, err := AggregateCapacity(dirs)
	if err != nil {
		t.Fatal(err)
	}
	a := aggs[CapacityKey{Transport: "msquic", Workload: "r10p1000", Regime: "wired"}]
	if a.Median != 11 {
		t.Fatalf("Median=%v, want 11 (still honest-based)", a.Median)
	}
	if !a.Conflicted {
		t.Fatal("censored bound above honest max should be conflicted")
	}
	if a.MaxLowerBound != 20 {
		t.Fatalf("MaxLowerBound=%v, want 20", a.MaxLowerBound)
	}
}

// N=1: 縮退(CI もIQR もなし、値そのもの)。
func TestAggregateCapacitySingleBlock(t *testing.T) {
	dirs := []string{
		writeCapacityBlock(t, []sweep.CellRecord{cell("websocket", "echo", "wired", 42, false, false)}),
	}
	aggs, err := AggregateCapacity(dirs)
	if err != nil {
		t.Fatal(err)
	}
	a := aggs[CapacityKey{Transport: "websocket", Workload: "echo", Regime: "wired"}]
	if a.N != 1 {
		t.Fatalf("N=%d, want 1", a.N)
	}
	if a.Median != 42 || a.IQLo != 42 || a.IQHi != 42 {
		t.Fatalf("degenerate stats: Median=%v IQLo=%v IQHi=%v, want all 42", a.Median, a.IQLo, a.IQHi)
	}
	if a.CILo != 0 || a.CIHi != 0 {
		t.Fatalf("N=1 should not produce a CI, got %v/%v", a.CILo, a.CIHi)
	}
}

func TestCapacityCITableRendersLowerBoundAndConflictMarkers(t *testing.T) {
	dirs := []string{
		writeCapacityBlock(t, []sweep.CellRecord{
			cell("enet", "echo", "wired", 10, false, false),
			cell("gns", "r60p200", "wired", 5, true, false),
		}),
		writeCapacityBlock(t, []sweep.CellRecord{
			cell("enet", "echo", "wired", 12, false, false),
			cell("gns", "r60p200", "wired", 8, true, false),
		}),
	}
	aggs, err := AggregateCapacity(dirs)
	if err != nil {
		t.Fatal(err)
	}
	table := CapacityCITable(aggs, "wired", false)
	if table == "" {
		t.Fatal("empty table")
	}
	if !strings.Contains(table, "enet") || !strings.Contains(table, "gns") {
		t.Fatalf("table missing expected columns:\n%s", table)
	}
	if !strings.Contains(table, "≥8") {
		t.Fatalf("table missing lower-bound marker for all-censored cell:\n%s", table)
	}
}
