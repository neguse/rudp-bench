package block

import (
	"fmt"

	"golang.org/x/sys/unix"

	"github.com/neguse/rudp-bench/orchestrator/rig"
)

// pinInstrumentCPUs は orchestrator 自身の affinity を rig 宣言の
// instrument_cpus に絞る。SUT/farm の taskset は子プロセス起動時に明示される
// ため影響しない。宣言の無い rig(home 等)では何もしない。
func pinInstrumentCPUs(r rig.Rig) error {
	if r.InstrumentCPUs == "" {
		return fmt.Errorf("rig %s declares no instrument_cpus", r.Name)
	}
	cpus, err := rig.ParseCPUSet(r.InstrumentCPUs)
	if err != nil {
		return fmt.Errorf("instrument_cpus: %w", err)
	}
	var set unix.CPUSet
	for _, c := range cpus {
		set.Set(c)
	}
	return unix.SchedSetaffinity(0, &set)
}
