package result

import "testing"

func TestBinCount(t *testing.T) {
	if BinCount != 78901 {
		t.Fatalf("BinCount = %d, want 78901", BinCount)
	}
}

func TestBinUpperBoundUS(t *testing.T) {
	// Exact range: index maps directly.
	if got := BinUpperBoundUS(0); got != 0 {
		t.Errorf("BinUpperBoundUS(0) = %d, want 0", got)
	}
	if got := BinUpperBoundUS(5000); got != 5000 {
		t.Errorf("BinUpperBoundUS(5000) = %d, want 5000", got)
	}
	if got := BinUpperBoundUS(ExactBins - 1); got != ExactMaxUS {
		t.Errorf("BinUpperBoundUS(%d) = %d, want %d", ExactBins-1, got, ExactMaxUS)
	}

	// Fine range: first fine bin.
	got := BinUpperBoundUS(ExactBins)
	want := ExactMaxUS + 1*FineBinUS // 10100
	if got != want {
		t.Errorf("BinUpperBoundUS(%d) = %d, want %d", ExactBins, got, want)
	}

	// Fine range: last fine bin.
	got = BinUpperBoundUS(ExactBins + FineBins - 1)
	want = ExactMaxUS + FineBins*FineBinUS // 1_000_000
	if got != want {
		t.Errorf("BinUpperBoundUS(%d) = %d, want %d", ExactBins+FineBins-1, got, want)
	}

	// Coarse range: first coarse bin.
	got = BinUpperBoundUS(ExactBins + FineBins)
	want = FineMaxUS + 1*CoarseBinUS // 1_001_000
	if got != want {
		t.Errorf("BinUpperBoundUS(%d) = %d, want %d", ExactBins+FineBins, got, want)
	}
}

func TestPercentileEmpty(t *testing.T) {
	h := &Histogram{}
	if got := h.PercentileUS(0.50); got != 0 {
		t.Errorf("PercentileUS(0.50) on empty = %d, want 0", got)
	}
	if got := h.PercentileUS(0.99); got != 0 {
		t.Errorf("PercentileUS(0.99) on empty = %d, want 0", got)
	}
}

func TestPercentileSimple(t *testing.T) {
	h := &Histogram{}
	h.Count = 100
	// Put 100 samples at bin index 500 (exact range, so upper bound = 500 us).
	h.Bins[500] = 100
	if got := h.PercentileUS(0.50); got != 500 {
		t.Errorf("p50 = %d, want 500", got)
	}
	if got := h.PercentileUS(0.99); got != 500 {
		t.Errorf("p99 = %d, want 500", got)
	}
}
