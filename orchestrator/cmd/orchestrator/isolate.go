package main

import (
	"fmt"
	"os"
	"os/exec"
	"strings"
)

// v1 の役割隔離(internal/runner/isolate.go)の v2 移植。root で直接実行する。
// レイアウト(5750GE 8C16T): OS/background 0-2,8-10 / client 3-6,11-14 / server 7,15。
// bench プロセス側の割当は RunConfig の server_cpus / client_cpus(taskset)で行い、
// ここでは OS 側 slice の退避・IRQ affinity・governor を設定する。
const (
	osCPUs    = "0-2,8-10"
	benchCPUs = "3 4 5 6 7 11 12 13 14 15"
	allCPUs   = "0-15"
)

func isolateMain(args []string) {
	if len(args) != 1 || (args[0] != "setup" && args[0] != "teardown") {
		fmt.Fprintln(os.Stderr, "usage: orchestrator isolate setup|teardown")
		os.Exit(1)
	}
	if os.Geteuid() != 0 {
		fmt.Fprintln(os.Stderr, "isolate requires root")
		os.Exit(1)
	}
	var script []string
	if args[0] == "setup" {
		script = []string{
			`systemctl set-property -- system.slice AllowedCPUs=` + osCPUs,
			`systemctl set-property -- user.slice   AllowedCPUs=` + osCPUs,
			`systemctl set-property -- init.scope   AllowedCPUs=` + osCPUs,
			`for f in /proc/irq/*/smp_affinity_list; do echo ` + osCPUs + ` > "$f" 2>/dev/null || true; done`,
			`for cpu in ` + benchCPUs + `; do gov=/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor; [ -e "$gov" ] && echo performance > "$gov" 2>/dev/null || true; done`,
			`cpupower -c ` + strings.ReplaceAll(benchCPUs, " ", ",") + ` idle-set -D 1 >/dev/null 2>&1 || true`,
		}
	} else {
		script = []string{
			`systemctl set-property -- system.slice AllowedCPUs=`,
			`systemctl set-property -- user.slice   AllowedCPUs=`,
			`systemctl set-property -- init.scope   AllowedCPUs=`,
			`for f in /proc/irq/*/smp_affinity_list; do echo ` + allCPUs + ` > "$f" 2>/dev/null || true; done`,
			`cpupower -c ` + strings.ReplaceAll(benchCPUs, " ", ",") + ` idle-set -E >/dev/null 2>&1 || true`,
		}
	}
	cmd := exec.Command("bash", "-c", strings.Join(script, " && "))
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	exitOnErr(cmd.Run())
	fmt.Fprintf(os.Stderr, "isolate %s done\n", args[0])
}
