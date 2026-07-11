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

func TestCountDropsInRange(t *testing.T) {
	// 128-bit trace: word0 has bits 0 and 64..127 pattern via word1.
	words := []uint64{0b1011, 0xF000000000000000}
	// total popcount = 3 + 4 = 7
	cases := []struct {
		from, to uint64
		want     uint64
	}{
		{0, 0, 0},
		{0, 4, 3},        // bits 0,1,3
		{1, 4, 2},        // bits 1,3
		{4, 64, 0},       // clear区間
		{60, 128, 4},     // word1 上位 4 bit
		{0, 128, 7},      // 1 周
		{0, 256, 14},     // 2 周
		{124, 132, 4 + 3}, // 巡回: 124-127 の 4 bit + 0-3 の 3 bit
	}
	for _, c := range cases {
		got, err := CountDropsInRange(words, c.from, c.to)
		if err != nil {
			t.Fatalf("[%d,%d): %v", c.from, c.to, err)
		}
		if got != c.want {
			t.Fatalf("[%d,%d) = %d, want %d", c.from, c.to, got, c.want)
		}
	}
	if _, err := CountDropsInRange([]uint64{1, 2, 3}, 0, 1); err == nil {
		t.Fatal("non power-of-two trace accepted")
	}
	if _, err := CountDropsInRange(words, 5, 4); err == nil {
		t.Fatal("inverted range accepted")
	}
}

func TestCountDropsMatchesGenerate(t *testing.T) {
	words, realized, err := Generate(42, 1.0, 0, 1<<12)
	if err != nil {
		t.Fatal(err)
	}
	full, err := CountDropsInRange(words, 0, 1<<12)
	if err != nil {
		t.Fatal(err)
	}
	if float64(full)/float64(1<<12)*100 != realized {
		t.Fatalf("popcount %d does not match realized rate %g", full, realized)
	}
}
