package main

import (
	"flag"
	"fmt"
	"os"
	"os/exec"
	"strings"

	"github.com/neguse/rudp-bench/orchestrator/rig"
)

// v1 の役割隔離(internal/runner/isolate.go)の v2 移植。root で直接実行する。
// CPU レイアウトは rig 記述ファイル(orchestrator/rigs/*.json)から読む。
// bench プロセス側の割当は RunConfig の server_cpus / client_cpus(taskset)で
// 行い、ここでは OS 側 slice の退避・IRQ affinity・governor を設定する。
//
// 注意: setup 後は user.slice も OS コアに退避されるため、sweep/boundary/block は
// bench slice の scope で起動しないと taskset が EINVAL になる:
//   sudo systemd-run --scope --slice=bench.slice -p AllowedCPUs=<bench_cpus> \
//     -p CPUWeight=10000 --quiet ./build-v2/orchestrator block -config ...
func isolateMain(args []string) {
	fs := flag.NewFlagSet("isolate", flag.ExitOnError)
	rigPath := fs.String("rig", "orchestrator/rigs/home.json", "rig description JSON")
	exitOnErr(fs.Parse(args))
	rest := fs.Args()
	if len(rest) != 1 || (rest[0] != "setup" && rest[0] != "teardown") {
		fmt.Fprintln(os.Stderr, "usage: orchestrator isolate [-rig FILE] setup|teardown")
		os.Exit(1)
	}
	if os.Geteuid() != 0 {
		fmt.Fprintln(os.Stderr, "isolate requires root")
		os.Exit(1)
	}
	r, err := rig.Load(*rigPath)
	exitOnErr(err)

	var script []string
	if rest[0] == "setup" {
		script = []string{
			`systemctl set-property -- system.slice AllowedCPUs=` + r.OSCPUs,
			`systemctl set-property -- user.slice   AllowedCPUs=` + r.OSCPUs,
			`systemctl set-property -- init.scope   AllowedCPUs=` + r.OSCPUs,
			`for f in /proc/irq/*/smp_affinity_list; do echo ` + r.OSCPUs + ` > "$f" 2>/dev/null || true; done`,
			// governor は全 online CPU に一律適用で足りる(OS コア側も害はない)
			`for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > "$gov" 2>/dev/null || true; done`,
			`cpupower -c ` + r.BenchCPUs + ` idle-set -D 1 >/dev/null 2>&1 || true`,
		}
	} else {
		script = []string{
			`systemctl set-property -- system.slice AllowedCPUs=`,
			`systemctl set-property -- user.slice   AllowedCPUs=`,
			`systemctl set-property -- init.scope   AllowedCPUs=`,
			`for f in /proc/irq/*/smp_affinity_list; do echo ` + r.AllCPUs + ` > "$f" 2>/dev/null || true; done`,
			`cpupower -c ` + r.BenchCPUs + ` idle-set -E >/dev/null 2>&1 || true`,
		}
	}
	cmd := exec.Command("bash", "-c", strings.Join(script, " && "))
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	exitOnErr(cmd.Run())
	fmt.Fprintf(os.Stderr, "isolate %s done (rig=%s)\n", rest[0], r.Name)
}
