package losstrace

import (
	"os"
	"testing"
)

// BPF_PROG_TEST_RUN でプログラム単体の drop 判定を検証する(要 root)。
func TestProgramDropLogic(t *testing.T) {
	if os.Geteuid() != 0 {
		t.Skip("requires root (BPF program load)")
	}

	// trace = 1010...(偶数番パケットを drop)、64 packets で一巡
	words := []uint64{0x5555555555555555}
	prog, maps, err := loadProgram(len(words))
	if err != nil {
		t.Fatal(err)
	}
	defer prog.Close()
	defer maps.state.Close()
	defer maps.trace.Close()
	for i, w := range words {
		if err := maps.trace.Put(uint32(i), w); err != nil {
			t.Fatal(err)
		}
	}

	pkt := make([]byte, 64) // ダミー ethernet フレーム
	for i := 0; i < 128; i++ {
		ret, _, err := prog.Test(pkt)
		if err != nil {
			t.Fatalf("test run %d: %v", i, err)
		}
		wantDrop := i%2 == 0 // bit0 が立っている → 偶数番 drop
		if wantDrop && ret != 2 {
			t.Fatalf("packet %d: ret=%d, want 2 (drop)", i, ret)
		}
		if !wantDrop && ret != 0 {
			t.Fatalf("packet %d: ret=%d, want 0 (pass)", i, ret)
		}
	}
	var counter uint64
	if err := maps.state.Lookup(uint32(0), &counter); err != nil {
		t.Fatal(err)
	}
	if counter != 128 {
		t.Fatalf("state counter = %d, want 128", counter)
	}
}
