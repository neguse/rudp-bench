package aggregate

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/neguse/rudp-bench/orchestrator/run"
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
		Transport:          transport,
		Workload:           workload,
		Regime:             regime,
		ComparisonIdentity: "comparison-v1",
		CellCapacity: sweep.CellCapacity{
			Capacity:     capacity,
			Censored:     censored,
			RangeLimited: rangeLimited,
		},
	}
}

func TestAggregateCapacityRejectsComparisonIdentityMismatch(t *testing.T) {
	first := cell("enet", "echo", "wired", 10, false, false)
	second := cell("enet", "echo", "wired", 12, false, false)
	second.ComparisonIdentity = "different-treatment"
	_, err := AggregateCapacity([]string{
		writeCapacityBlock(t, []sweep.CellRecord{first}),
		writeCapacityBlock(t, []sweep.CellRecord{second}),
	})
	if err == nil || !strings.Contains(err.Error(), "comparison identity mismatch") {
		t.Fatalf("expected comparison identity mismatch, got %v", err)
	}
}

func TestAggregateCapacityRejectsTerminalOutcome(t *testing.T) {
	record := cell("enet", "echo", "wired", 0, false, false)
	record.Outcome = run.OutcomeUnsupported
	_, err := AggregateCapacity([]string{writeCapacityBlock(t, []sweep.CellRecord{record})})
	if err == nil || !strings.Contains(err.Error(), "terminal outcome UNSUPPORTED") {
		t.Fatalf("expected terminal outcome rejection, got %v", err)
	}
}

func TestAggregateCapacityRejectsMeasurementInvalid(t *testing.T) {
	record := cell("enet", "echo", "wired", 0, true, false)
	record.MeasurementInvalid = true
	_, err := AggregateCapacity([]string{writeCapacityBlock(t, []sweep.CellRecord{record})})
	if err == nil || !strings.Contains(err.Error(), "measurement_invalid") {
		t.Fatalf("expected measurement invalid rejection, got %v", err)
	}
}

func TestAggregateCapacityLegacySafety(t *testing.T) {
	legacy := cell("enet", "echo", "wired", 10, false, false)
	legacy.ComparisonIdentity = ""
	if _, err := AggregateCapacity([]string{writeCapacityBlock(t, []sweep.CellRecord{legacy})}); err != nil {
		t.Fatalf("single legacy input should remain readable: %v", err)
	}

	if _, err := AggregateCapacity([]string{
		writeCapacityBlock(t, []sweep.CellRecord{legacy}),
		writeCapacityBlock(t, []sweep.CellRecord{legacy}),
	}); err == nil || !strings.Contains(err.Error(), "multiple legacy") {
		t.Fatalf("expected multiple legacy inputs to be rejected, got %v", err)
	}

	modern := legacy
	modern.Workload = "reliable_echo"
	modern.ComparisonIdentity = "comparison-v1"
	if _, err := AggregateCapacity([]string{writeCapacityBlock(t, []sweep.CellRecord{legacy, modern})}); err == nil ||
		!strings.Contains(err.Error(), "cannot mix legacy") {
		t.Fatalf("expected mixed legacy/identified cells to be rejected, got %v", err)
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

func TestAggregateCapacityUsesScenarioName(t *testing.T) {
	record := cell("enet", "", "local", 32, false, false)
	record.Scenario = "authoritative-smoke"
	aggs, err := AggregateCapacity([]string{writeCapacityBlock(t, []sweep.CellRecord{record})})
	if err != nil {
		t.Fatal(err)
	}
	key := CapacityKey{Transport: "enet", Workload: "authoritative-smoke", Regime: "local"}
	if _, ok := aggs[key]; !ok {
		t.Fatalf("scenario key missing: %+v", aggs)
	}
	if table := CapacityCITable(aggs, "local", false); !strings.Contains(table, "| authoritative-smoke |") {
		t.Fatalf("scenario row missing:\n%s", table)
	}
}

// 全ブロック censored: sample medianの保守的下限はlower endpointのmedian。
func TestAggregateCapacityCensoredOnlyUsesMedianLowerEndpoint(t *testing.T) {
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
	if a.LowerBound != 6 {
		t.Fatalf("LowerBound=%v, want median(5,8,6)=6", a.LowerBound)
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

// mixedでもcensored observationをexact値として捨てず、medianの下限だけを出す。
func TestAggregateCapacityMixedReportsMedianLowerBound(t *testing.T) {
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
	if a.Median != 0 || a.LowerBound != 10.5 {
		t.Fatalf("Median=%v LowerBound=%v, want 0/10.5", a.Median, a.LowerBound)
	}
	if a.Conflicted {
		t.Fatal("censored bound below honest max should not be conflicted")
	}
	if a.MaxLowerBound != 0 {
		t.Fatalf("MaxLowerBound=%v, want 0 (not set when not conflicted)", a.MaxLowerBound)
	}
}

func TestAggregateCapacityMixedHighCensorIsStillLowerBound(t *testing.T) {
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
	if a.Median != 0 || a.LowerBound != 11.5 {
		t.Fatalf("Median=%v LowerBound=%v, want 0/11.5", a.Median, a.LowerBound)
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
	if !strings.Contains(table, "≥6.5") {
		t.Fatalf("table missing lower-bound marker for all-censored cell:\n%s", table)
	}
}

func TestAggregateCapacityRejectsDuplicateInputCampaignAndEvidence(t *testing.T) {
	record := cell("enet", "echo", "wired", 10, false, false)
	record.CampaignIdentity = "campaign"
	record.EvidenceIDs = []string{"acquisition"}
	first := writeCapacityBlock(t, []sweep.CellRecord{record})
	if _, err := AggregateCapacity([]string{first, first}); err == nil || !strings.Contains(err.Error(), "duplicate capacity input") {
		t.Fatalf("duplicate dir error = %v", err)
	}
	second := writeCapacityBlock(t, []sweep.CellRecord{record})
	if _, err := AggregateCapacity([]string{first, second}); err == nil || !strings.Contains(err.Error(), "repeats campaign_identity") {
		t.Fatalf("duplicate campaign error = %v", err)
	}
	record.CampaignIdentity = "other-campaign"
	third := writeCapacityBlock(t, []sweep.CellRecord{record})
	if _, err := AggregateCapacity([]string{first, third}); err == nil || !strings.Contains(err.Error(), "reuses acquisition") {
		t.Fatalf("duplicate acquisition error = %v", err)
	}
}
