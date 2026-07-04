package aggregate

import (
	"math/rand"
	"testing"
)

func TestBootstrapMedianCIDeterministic(t *testing.T) {
	values := []float64{10, 12, 11, 14, 9, 13}
	lo1, hi1 := BootstrapMedianCI(values, 500, 0.95, rand.New(rand.NewSource(42)))
	lo2, hi2 := BootstrapMedianCI(values, 500, 0.95, rand.New(rand.NewSource(42)))
	if lo1 != lo2 || hi1 != hi2 {
		t.Fatalf("same seed gave different results: (%v,%v) vs (%v,%v)", lo1, hi1, lo2, hi2)
	}
	if lo1 > hi1 {
		t.Fatalf("lo > hi: %v > %v", lo1, hi1)
	}
}

func TestBootstrapMedianCIEmpty(t *testing.T) {
	lo, hi := BootstrapMedianCI(nil, 500, 0.95, rand.New(rand.NewSource(1)))
	if lo != 0 || hi != 0 {
		t.Fatalf("empty values: got (%v,%v), want (0,0)", lo, hi)
	}
}

func TestBootstrapMedianCISingleValueCollapses(t *testing.T) {
	lo, hi := BootstrapMedianCI([]float64{42}, 200, 0.95, rand.New(rand.NewSource(7)))
	if lo != 42 || hi != 42 {
		t.Fatalf("single value: got (%v,%v), want (42,42)", lo, hi)
	}
}
