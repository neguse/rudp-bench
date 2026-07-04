// Package rig は実行環境の記述(design spec「rig 抽象」)。CPU レイアウトを
// コードから外出しし、home / aws-metal を同じ orchestrator で回せるようにする。
package rig

import (
	"encoding/json"
	"fmt"
	"os"
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
}

func Load(path string) (Rig, error) {
	var r Rig
	data, err := os.ReadFile(path)
	if err != nil {
		return r, err
	}
	if err := json.Unmarshal(data, &r); err != nil {
		return r, fmt.Errorf("%s: %w", path, err)
	}
	for name, v := range map[string]string{
		"name": r.Name, "os_cpus": r.OSCPUs, "bench_cpus": r.BenchCPUs,
		"client_cpus": r.ClientCPUs, "server_cpus": r.ServerCPUs, "all_cpus": r.AllCPUs,
	} {
		if v == "" {
			return r, fmt.Errorf("%s: %s is required", path, name)
		}
	}
	return r, nil
}
