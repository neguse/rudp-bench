package losstrace

import "testing"

// 同一 seed は同一 trace、異なる seed は異なる trace を生むこと(決定性)。
func TestGenerateDeterministic(t *testing.T) {
	a1, _, err := Generate(42, 3, 16, 1<<16)
	if err != nil {
		t.Fatal(err)
	}
	a2, _, err := Generate(42, 3, 16, 1<<16)
	if err != nil {
		t.Fatal(err)
	}
	b, _, err := Generate(43, 3, 16, 1<<16)
	if err != nil {
		t.Fatal(err)
	}
	if len(a1) != (1<<16)/64 {
		t.Fatalf("words = %d, want %d", len(a1), (1<<16)/64)
	}
	same := true
	for i := range a1 {
		if a1[i] != a2[i] {
			t.Fatalf("same seed produced different traces at word %d", i)
		}
		if a1[i] != b[i] {
			same = false
		}
	}
	if same {
		t.Fatal("different seeds produced identical traces")
	}
}

// 実現 loss 率が設定値に十分近いこと(1M packets で ±10% 相対)。
func TestGenerateRealizedRate(t *testing.T) {
	for _, tc := range []struct {
		pct   float64
		burst float64
	}{
		{3, 16}, {1, 4}, {0.1, 1}, {1, 1},
	} {
		_, realized, err := Generate(7, tc.pct, tc.burst, 1<<20)
		if err != nil {
			t.Fatalf("pct=%g burst=%g: %v", tc.pct, tc.burst, err)
		}
		if realized < tc.pct*0.9 || realized > tc.pct*1.1 {
			t.Fatalf("pct=%g burst=%g: realized %g%% outside ±10%%", tc.pct, tc.burst, realized)
		}
	}
}

// バースト長の平均が指定に近いこと(gemodel の意味論の確認)。
func TestGenerateBurstLength(t *testing.T) {
	words, _, err := Generate(11, 3, 16, 1<<20)
	if err != nil {
		t.Fatal(err)
	}
	bits := len(words) * 64
	bursts, dropped := 0, 0
	prev := false
	for i := 0; i < bits; i++ {
		cur := words[i/64]&(1<<(uint(i)%64)) != 0
		if cur {
			dropped++
			if !prev {
				bursts++
			}
		}
		prev = cur
	}
	if bursts == 0 {
		t.Fatal("no bursts generated")
	}
	avg := float64(dropped) / float64(bursts)
	if avg < 16*0.8 || avg > 16*1.2 {
		t.Fatalf("average burst length %.2f, want ~16 (±20%%)", avg)
	}
}

func TestGenerateValidation(t *testing.T) {
	if _, _, err := Generate(1, 3, 16, 1000); err == nil {
		t.Fatal("non-power-of-two bits should fail")
	}
	if _, _, err := Generate(1, 0, 16, 1<<10); err == nil {
		t.Fatal("zero loss should fail")
	}
	if _, _, err := Generate(1, 100, 16, 1<<10); err == nil {
		t.Fatal("100%% loss should fail")
	}
}
