package sweep

import "testing"

func pt(conns int, ok, censored bool, cause string) PointRecord {
	return PointRecord{Transport: "t", Workload: "w", Regime: "r", Conns: conns,
		Judgment: Judgment{OK: ok, Censored: censored, Cause: cause}}
}

func TestDeriveCellCensoredBeforeFail(t *testing.T) {
	cell := deriveCell([]PointRecord{
		pt(32, true, false, ""),
		pt(64, true, false, ""),
		pt(66, false, true, "farm_limited: pacing stall"),
		pt(128, false, false, "staleness"),
	})
	if !cell.Censored || cell.Capacity != 64 || cell.BreakConns != 0 {
		t.Fatalf("cell = %+v", cell)
	}
}

func TestDeriveCellPlainBreak(t *testing.T) {
	cell := deriveCell([]PointRecord{
		pt(1, true, false, ""),
		pt(64, true, false, ""),
		pt(96, false, false, "delivery_lt"),
		pt(80, true, false, ""),
	})
	if cell.Censored || cell.Capacity != 80 || cell.BreakConns != 96 || cell.BreakCause != "delivery_lt" {
		t.Fatalf("cell = %+v", cell)
	}
}

func TestDeriveCellAllOK(t *testing.T) {
	cell := deriveCell([]PointRecord{pt(1, true, false, ""), pt(1024, true, false, "")})
	if !cell.RangeLimited || cell.Capacity != 1024 {
		t.Fatalf("cell = %+v", cell)
	}
}
