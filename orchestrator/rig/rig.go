// Package rig は実行環境の記述(design spec「rig 抽象」)。CPU レイアウトを
// コードから外出しし、home / aws-metal を同じ orchestrator で回せるようにする。
package rig

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"sort"
	"strconv"
	"strings"
)

type Rig struct {
	Name string `json:"name"`
	// OS/background を退避するコア(isolate setup が slice に設定)
	OSCPUs string `json:"os_cpus"`
	// bench 全体(sweep プロセスを載せる scope 用)
	BenchCPUs string `json:"bench_cpus"`
	// 役割別(RunConfig の client_cpus / server_cpus に入る)
	ClientCPUs string `json:"client_cpus"`
	ServerCPUs string `json:"server_cpus"`
	// teardown 時に IRQ affinity を戻す全コア
	AllCPUs string `json:"all_cpus"`
	// Reference measurement preflight. These fields are optional so existing
	// development rigs remain readable; doctor reports WARN when unspecified.
	ExpectedClocksource        string `json:"expected_clocksource,omitempty"`
	RequirePerformanceGovernor bool   `json:"require_performance_governor,omitempty"`
	// cpufreq を一切露出しない platform(EC2 Graviton 等)は
	// require_performance_governor の代わりにこれを宣言する。
	// cpufreq が見えたら FAIL(前提が崩れた証拠)。
	ExpectFixedFrequency bool   `json:"expect_fixed_frequency,omitempty"`
	RequireIsolation     bool   `json:"require_isolation,omitempty"`
	MinNoFile            uint64 `json:"min_nofile,omitempty"`
	// farm 凍結構成(client rcvbuf 4MB 明示)の前提となる kernel 上限。
	// 未宣言の rig では doctor が WARN を出すだけに留める
	MinRmemMax uint64 `json:"min_rmem_max,omitempty"`
	MinWmemMax uint64 `json:"min_wmem_max,omitempty"`
}

func Load(path string) (Rig, error) {
	var r Rig
	data, err := os.ReadFile(path)
	if err != nil {
		return r, err
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&r); err != nil {
		return r, fmt.Errorf("%s: %w", path, err)
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		return r, fmt.Errorf("%s: expected exactly one JSON object", path)
	}
	for name, v := range map[string]string{
		"name": r.Name, "os_cpus": r.OSCPUs, "bench_cpus": r.BenchCPUs,
		"client_cpus": r.ClientCPUs, "server_cpus": r.ServerCPUs, "all_cpus": r.AllCPUs,
	} {
		if v == "" {
			return r, fmt.Errorf("%s: %s is required", path, name)
		}
	}
	if err := r.Validate(); err != nil {
		return r, fmt.Errorf("%s: %w", path, err)
	}
	return r, nil
}

func (r Rig) Validate() error {
	sets := map[string][]int{}
	for name, value := range map[string]string{
		"os_cpus": r.OSCPUs, "bench_cpus": r.BenchCPUs, "client_cpus": r.ClientCPUs,
		"server_cpus": r.ServerCPUs, "all_cpus": r.AllCPUs,
	} {
		cpus, err := ParseCPUSet(value)
		if err != nil || len(cpus) == 0 {
			return fmt.Errorf("%s: invalid CPU set %q", name, value)
		}
		sets[name] = cpus
	}
	if overlap(sets["os_cpus"], sets["bench_cpus"]) {
		return fmt.Errorf("os_cpus and bench_cpus must be disjoint")
	}
	if overlap(sets["client_cpus"], sets["server_cpus"]) {
		return fmt.Errorf("client_cpus and server_cpus must be disjoint")
	}
	if !subset(sets["client_cpus"], sets["bench_cpus"]) || !subset(sets["server_cpus"], sets["bench_cpus"]) {
		return fmt.Errorf("client_cpus and server_cpus must be subsets of bench_cpus")
	}
	if !sameSet(union(sets["os_cpus"], sets["bench_cpus"]), sets["all_cpus"]) {
		return fmt.Errorf("os_cpus union bench_cpus must equal all_cpus")
	}
	if !sameSet(union(sets["client_cpus"], sets["server_cpus"]), sets["bench_cpus"]) {
		return fmt.Errorf("client_cpus union server_cpus must equal bench_cpus")
	}
	if r.RequirePerformanceGovernor && r.ExpectFixedFrequency {
		return fmt.Errorf("require_performance_governor and expect_fixed_frequency are mutually exclusive")
	}
	return nil
}

func ParseCPUSet(value string) ([]int, error) {
	seen := map[int]bool{}
	value = strings.ReplaceAll(value, " ", ",")
	for _, part := range strings.Split(value, ",") {
		part = strings.TrimSpace(part)
		if part == "" {
			continue
		}
		loText, hiText, ranged := strings.Cut(part, "-")
		lo, err := strconv.Atoi(loText)
		if err != nil || lo < 0 {
			return nil, fmt.Errorf("invalid CPU set %q", value)
		}
		hi := lo
		if ranged {
			hi, err = strconv.Atoi(hiText)
			if err != nil || hi < lo {
				return nil, fmt.Errorf("invalid CPU set %q", value)
			}
		}
		for cpu := lo; cpu <= hi; cpu++ {
			seen[cpu] = true
		}
	}
	out := make([]int, 0, len(seen))
	for cpu := range seen {
		out = append(out, cpu)
	}
	sort.Ints(out)
	return out, nil
}

func sameSet(a, b []int) bool {
	return len(a) == len(b) && subset(a, b)
}

func subset(a, b []int) bool {
	have := map[int]bool{}
	for _, cpu := range b {
		have[cpu] = true
	}
	for _, cpu := range a {
		if !have[cpu] {
			return false
		}
	}
	return true
}

func overlap(a, b []int) bool {
	have := map[int]bool{}
	for _, cpu := range a {
		have[cpu] = true
	}
	for _, cpu := range b {
		if have[cpu] {
			return true
		}
	}
	return false
}

func union(a, b []int) []int {
	seen := map[int]bool{}
	for _, values := range [][]int{a, b} {
		for _, cpu := range values {
			seen[cpu] = true
		}
	}
	out := make([]int, 0, len(seen))
	for cpu := range seen {
		out = append(out, cpu)
	}
	sort.Ints(out)
	return out
}

func IsSubset(value, container string) bool {
	a, err := ParseCPUSet(value)
	if err != nil {
		return false
	}
	b, err := ParseCPUSet(container)
	return err == nil && subset(a, b)
}

func Intersects(a, b string) bool {
	left, err := ParseCPUSet(a)
	if err != nil {
		return false
	}
	right, err := ParseCPUSet(b)
	return err == nil && overlap(left, right)
}
