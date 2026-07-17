package block

import (
	"testing"

	"golang.org/x/sys/unix"

	"github.com/neguse/rudp-bench/orchestrator/rig"
)

func TestPinInstrumentCPUsUsesSpareBenchCPUs(t *testing.T) {
	var orig unix.CPUSet
	if err := unix.SchedGetaffinity(0, &orig); err != nil {
		t.Fatalf("get affinity: %v", err)
	}
	defer func() {
		if err := unix.SchedSetaffinity(0, &orig); err != nil {
			t.Fatalf("restore affinity: %v", err)
		}
	}()

	r := rig.Rig{BenchCPUs: "0-1", ServerCPUs: "1", ClientCPUs: "1"}
	if err := pinInstrumentCPUs(r); err != nil {
		t.Fatalf("pin: %v", err)
	}
	var got unix.CPUSet
	if err := unix.SchedGetaffinity(0, &got); err != nil {
		t.Fatalf("get pinned affinity: %v", err)
	}
	if !got.IsSet(0) || got.IsSet(1) || got.Count() != 1 {
		t.Fatalf("want affinity {0}, got count=%d", got.Count())
	}
}

func TestPinInstrumentCPUsErrorsWithoutSpare(t *testing.T) {
	r := rig.Rig{BenchCPUs: "0-1", ServerCPUs: "0", ClientCPUs: "1"}
	if err := pinInstrumentCPUs(r); err == nil {
		t.Fatal("want error when no spare bench CPU exists")
	}
}
