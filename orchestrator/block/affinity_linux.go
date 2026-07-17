package block

import (
	"fmt"

	"golang.org/x/sys/unix"

	"github.com/neguse/rudp-bench/orchestrator/rig"
)

// pinInstrumentCPUs は orchestrator 自身の affinity を
// bench_cpus − server_cpus − client_cpus に絞る。SUT/farm の taskset は
// 子プロセス起動時に明示されるため影響しない。空き CPU が無い rig
// (home 等)では何もしない。
func pinInstrumentCPUs(r rig.Rig) error {
	bench, err := rig.ParseCPUSet(r.BenchCPUs)
	if err != nil {
		return fmt.Errorf("bench_cpus: %w", err)
	}
	used := map[int]bool{}
	for _, field := range []string{r.ServerCPUs, r.ClientCPUs} {
		cpus, err := rig.ParseCPUSet(field)
		if err != nil {
			return err
		}
		for _, c := range cpus {
			used[c] = true
		}
	}
	var set unix.CPUSet
	free := 0
	for _, c := range bench {
		if !used[c] {
			set.Set(c)
			free++
		}
	}
	if free == 0 {
		return fmt.Errorf("no spare bench CPUs (bench=%s server=%s client=%s)",
			r.BenchCPUs, r.ServerCPUs, r.ClientCPUs)
	}
	return unix.SchedSetaffinity(0, &set)
}
