package sentinel

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"

	"github.com/neguse/rudp-bench/orchestrator/sweep"
)

func writeReference(t *testing.T, cells []sweep.CellRecord) string {
	t.Helper()
	dir := t.TempDir()
	data, _ := json.Marshal(struct {
		Cells []sweep.CellRecord `json:"cells"`
	}{cells})
	if err := os.WriteFile(filepath.Join(dir, "capacity.json"), data, 0o644); err != nil {
		t.Fatal(err)
	}
	return dir
}

func cell(transport, workload string, cap_, breakConns int, censored bool) sweep.CellRecord {
	return sweep.CellRecord{Transport: transport, Workload: workload, Regime: "wired",
		CellCapacity: sweep.CellCapacity{Capacity: cap_, BreakConns: breakConns, Censored: censored}}
}

func TestPlanProbes(t *testing.T) {
	ref := writeReference(t, []sweep.CellRecord{
		cell("enet", "r20p128", 115, 116, false), // 正直 break → 2点
		cell("gns", "r20p128", 64, 0, true),      // censored → 下限1点
		cell("litenetlib", "r20p128", 0, 4, false), // capacity 0 → break 1点
	})
	cfg := Config{
		Workloads: []string{"r20p128"},
		Transports: map[string]sweep.TransportSpec{
			"enet": {}, "gns": {}, "litenetlib": {},
		},
		Regimes: []RegimeSpec{{Name: "wired", ReferenceDir: ref}},
	}
	probes, err := PlanProbes(cfg)
	if err != nil {
		t.Fatal(err)
	}
	got := map[string]Probe{}
	for _, p := range probes {
		got[p.Transport+p.Expect] = p
	}
	if len(probes) != 4 {
		t.Fatalf("want 4 probes, got %d: %+v", len(probes), probes)
	}
	if p := got["enetok"]; p.Conns != 104 { // 0.9×115
		t.Fatalf("enet ok probe: %+v", p)
	}
	if p := got["enetfail"]; p.Conns != 133 { // 1.15×116
		t.Fatalf("enet fail probe: %+v", p)
	}
	if p := got["gnsbound"]; p.Conns != 64 {
		t.Fatalf("gns bound probe: %+v", p)
	}
	if p := got["litenetlibfail"]; p.Conns != 4 {
		t.Fatalf("litenetlib fail probe: %+v", p)
	}
}

func TestJudgeProbe(t *testing.T) {
	mk := func(expect string, ok, censored bool) Probe {
		p := Probe{Expect: expect, Judgment: sweep.Judgment{OK: ok, Censored: censored, Cause: "x"}}
		judgeProbe(&p)
		return p
	}
	cases := []struct {
		p    Probe
		want string
	}{
		{mk("ok", true, false), "PASS"},
		{mk("ok", false, false), "DRIFT"},
		{mk("ok", false, true), "DRIFT"},
		{mk("fail", false, false), "PASS"},
		{mk("fail", true, false), "DRIFT"}, // 上振れも drift として報告(良い方向でも基準更新が必要)
		{mk("bound", true, false), "PASS"},
		{mk("bound", false, true), "PASS"},
		{mk("bound", false, false), "DRIFT"},
	}
	for i, c := range cases {
		if c.p.Outcome != c.want {
			t.Fatalf("case %d: outcome=%s want=%s (%+v)", i, c.p.Outcome, c.want, c.p)
		}
	}
}
