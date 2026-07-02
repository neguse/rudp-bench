package runner

import (
	"fmt"
	"os"
	"os/exec"
	"strconv"
	"strings"
)

// parseNetemNumber validates that s is a non-negative number for the named
// netem parameter and returns it unchanged (tc へは文字列のまま渡す).
func parseNetemNumber(name, s string) (string, error) {
	v, err := strconv.ParseFloat(s, 64)
	if err != nil {
		return "", fmt.Errorf("netem %s: %q is not a number", name, s)
	}
	if v < 0 {
		return "", fmt.Errorf("netem %s: %q must be >= 0", name, s)
	}
	return s, nil
}

// ApplyNetem applies a netem qdisc to the loopback interface.
// args format: "delay_ms [jitter_ms [loss_pct [limit_pkts]]]".
//
// 注意: jitter > 0 の場合、netem は遅延のばらつきによりパケット並べ替え
// (reorder) を副作用として起こす。reliable チャネルは吸収するが、
// unreliable の delivery/RTT 統計には影響する。
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
	if len(parts) > 4 {
		return fmt.Errorf("netem args: too many fields %q (want 'delay_ms [jitter_ms [loss_pct [limit_pkts]]]')", args)
	}

	var err error
	if delayMs, err = parseNetemNumber("delay_ms", delayMs); err != nil {
		return err
	}
	if jitterMs, err = parseNetemNumber("jitter_ms", jitterMs); err != nil {
		return err
	}
	if lossPct, err = parseNetemNumber("loss_pct", lossPct); err != nil {
		return err
	}
	if loss, _ := strconv.ParseFloat(lossPct, 64); loss > 100 {
		return fmt.Errorf("netem loss_pct: %q must be <= 100", lossPct)
	}
	if n, err := strconv.Atoi(limitPkts); err != nil || n <= 0 {
		return fmt.Errorf("netem limit_pkts: %q must be a positive integer", limitPkts)
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

	writeQdiscLog(logDir, "netem_apply.txt")
	return nil
}

func ClearNetem(logDir string) {
	sudoTC("qdisc", "del", "dev", "lo", "root")
	writeQdiscLog(logDir, "netem_clear.txt")
}

// writeQdiscLog records the current qdisc state of lo into logDir/name.
// 失敗してもベンチ続行に支障はないため warning に留める。
func writeQdiscLog(logDir, name string) {
	if logDir == "" {
		return
	}
	logPath := logDir + "/" + name
	out, _ := exec.Command("tc", "qdisc", "show", "dev", "lo").CombinedOutput()
	if err := os.WriteFile(logPath, out, 0644); err != nil {
		fmt.Fprintf(os.Stderr, "warning: writing %s: %v\n", logPath, err)
	}
}

func sudoTC(args ...string) error {
	cmd := exec.Command("sudo", append([]string{"tc"}, args...)...)
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	return cmd.Run()
}
