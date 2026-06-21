package runner

import (
	"fmt"
	"os"
	"os/exec"
	"strings"
)

func ApplyNetem(args string, logDir string) error {
	parts := strings.Fields(args)
	if len(parts) < 1 {
		return fmt.Errorf("netem args require at least delay_ms")
	}
	delayMs := parts[0]
	jitterMs := "0"
	lossPct := "0"
	limitPkts := "100000"
	if len(parts) > 1 {
		jitterMs = parts[1]
	}
	if len(parts) > 2 {
		lossPct = parts[2]
	}
	if len(parts) > 3 {
		limitPkts = parts[3]
	}

	sudoTC("qdisc", "del", "dev", "lo", "root")

	tcArgs := []string{"qdisc", "add", "dev", "lo", "root", "netem",
		"limit", limitPkts, "delay", delayMs + "ms"}
	if jitterMs != "0" {
		tcArgs = append(tcArgs, jitterMs+"ms", "distribution", "normal")
	}
	if lossPct != "0" {
		tcArgs = append(tcArgs, "loss", lossPct+"%")
	}
	if err := sudoTC(tcArgs...); err != nil {
		return fmt.Errorf("netem apply: %w", err)
	}

	if logDir != "" {
		logPath := logDir + "/netem_apply.txt"
		out, _ := exec.Command("tc", "qdisc", "show", "dev", "lo").CombinedOutput()
		os.WriteFile(logPath, out, 0644)
	}
	return nil
}

func ClearNetem(logDir string) {
	sudoTC("qdisc", "del", "dev", "lo", "root")
	if logDir != "" {
		logPath := logDir + "/netem_clear.txt"
		out, _ := exec.Command("tc", "qdisc", "show", "dev", "lo").CombinedOutput()
		os.WriteFile(logPath, out, 0644)
	}
}

func sudoTC(args ...string) error {
	cmd := exec.Command("sudo", append([]string{"tc"}, args...)...)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	return cmd.Run()
}
