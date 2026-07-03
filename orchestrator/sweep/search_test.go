package sweep

import "testing"

func monotone(k int) func(int) (PointOutcome, error) {
	return func(conns int) (PointOutcome, error) {
		if conns <= k {
			return PointOutcome{OK: true}, nil
		}
		return PointOutcome{Cause: "delivery_lt below floor"}, nil
	}
}

func TestFindCapacityMonotone(t *testing.T) {
	cell, err := FindCapacity(monotone(37), 1, 512)
	if err != nil {
		t.Fatal(err)
	}
	if cell.Capacity != 37 || cell.BreakConns != 38 {
		t.Fatalf("capacity=%d break=%d, want 37/38", cell.Capacity, cell.BreakConns)
	}
	if cell.Censored || cell.RangeLimited {
		t.Fatalf("unexpected flags: %+v", cell)
	}
	if cell.BreakCause == "" {
		t.Fatal("break cause required")
	}
	// 指数 + 二分で点数は対数オーダー(全探索 512 に対して)
	if cell.Evaluated > 20 {
		t.Fatalf("too many evaluations: %d", cell.Evaluated)
	}
}

func TestFindCapacityFailAtMin(t *testing.T) {
	cell, err := FindCapacity(monotone(0), 1, 512)
	if err != nil {
		t.Fatal(err)
	}
	if cell.Capacity != 0 || cell.BreakConns != 1 {
		t.Fatalf("capacity=%d break=%d, want 0/1", cell.Capacity, cell.BreakConns)
	}
}

func TestFindCapacityRangeLimited(t *testing.T) {
	cell, err := FindCapacity(monotone(1 << 20), 1, 512)
	if err != nil {
		t.Fatal(err)
	}
	if cell.Capacity != 512 || !cell.RangeLimited {
		t.Fatalf("capacity=%d range_limited=%v, want 512/true", cell.Capacity, cell.RangeLimited)
	}
}

func TestFindCapacityCensored(t *testing.T) {
	eval := func(conns int) (PointOutcome, error) {
		if conns >= 100 {
			return PointOutcome{Censored: true, Cause: "farm_limited: attempted_ratio"}, nil
		}
		return PointOutcome{OK: true}, nil
	}
	cell, err := FindCapacity(eval, 1, 512)
	if err != nil {
		t.Fatal(err)
	}
	if !cell.Censored {
		t.Fatalf("expected censored: %+v", cell)
	}
	// capacity は最後に OK だった点(下限値)。break として報告しない
	if cell.Capacity != 64 || cell.BreakConns != 0 {
		t.Fatalf("capacity=%d break=%d, want 64/0", cell.Capacity, cell.BreakConns)
	}
}

func TestFindCapacityMinEqualsMax(t *testing.T) {
	cell, err := FindCapacity(monotone(10), 4, 4)
	if err != nil {
		t.Fatal(err)
	}
	if cell.Capacity != 4 || !cell.RangeLimited {
		t.Fatalf("capacity=%d range_limited=%v, want 4/true", cell.Capacity, cell.RangeLimited)
	}
}
