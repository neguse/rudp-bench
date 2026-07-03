package boundary

import (
	"testing"

	"github.com/neguse/rudp-bench/orchestrator/sweep"
)

func testConfig() Config {
	return Config{
		FloorConns: 4,
		Fractions:  []float64{0.25, 0.75},
	}
}

func TestLoadsForDerivesFromCapacity(t *testing.T) {
	capacity := map[string]sweep.CellRecord{
		"enet|r60p200": {CellCapacity: sweep.CellCapacity{Capacity: 90}},
	}
	loads := loadsFor(testConfig(), capacity, "enet", "r60p200")
	if len(loads) != 3 {
		t.Fatalf("want floor+2 loads, got %+v", loads)
	}
	if loads[0].Label != "floor" || loads[0].Conns != 4 {
		t.Fatalf("floor load wrong: %+v", loads[0])
	}
	if loads[1].Label != "q25" || loads[1].Conns != 23 { // 0.25×90 = 22.5 → 23
		t.Fatalf("q25 wrong: %+v", loads[1])
	}
	if loads[2].Label != "q75" || loads[2].Conns != 68 { // 0.75×90 = 67.5 → 68
		t.Fatalf("q75 wrong: %+v", loads[2])
	}
}

func TestLoadsForSkipsWhenCapacityUnusable(t *testing.T) {
	cfg := testConfig()
	// capacity 0(break at min)→ floor のみ
	capacity := map[string]sweep.CellRecord{"litenetlib|r60p200": {CellCapacity: sweep.CellCapacity{Capacity: 0}}}
	if loads := loadsFor(cfg, capacity, "litenetlib", "r60p200"); len(loads) != 1 {
		t.Fatalf("want floor only for capacity 0, got %+v", loads)
	}
	// capacity ≤ floor → floor のみ
	capacity = map[string]sweep.CellRecord{"gns|r60p1000": {CellCapacity: sweep.CellCapacity{Capacity: 4}}}
	if loads := loadsFor(cfg, capacity, "gns", "r60p1000"); len(loads) != 1 {
		t.Fatalf("want floor only for capacity<=floor, got %+v", loads)
	}
	// セルなし → floor のみ
	if loads := loadsFor(cfg, capacity, "unknown", "r60p200"); len(loads) != 1 {
		t.Fatalf("want floor only for missing cell, got %+v", loads)
	}
}

func TestLoadsForCensoredPropagates(t *testing.T) {
	capacity := map[string]sweep.CellRecord{
		"litenetlib|r20p1000": {CellCapacity: sweep.CellCapacity{Capacity: 64, Censored: true}},
	}
	loads := loadsFor(testConfig(), capacity, "litenetlib", "r20p1000")
	if len(loads) != 3 || !loads[1].CapacityCensored {
		t.Fatalf("censored flag must propagate to fraction loads: %+v", loads)
	}
	if loads[0].CapacityCensored {
		t.Fatalf("floor load is not capacity-based: %+v", loads[0])
	}
}

func TestPointKeyStable(t *testing.T) {
	k := pointKey("enet", "r60p200", 3, 16, "q25", 23)
	if k != "enet|r60p200|l3b16|q25|c23" {
		t.Fatalf("key = %q", k)
	}
}
