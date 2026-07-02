package runner

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"os/user"
	"strconv"
	"strings"
	"syscall"
	"time"
)

var envPassthrough = []string{
	"ENET_NO_THROTTLE", "ENET_BATCH_POLL", "ENET_POOL",
	"ENET_RCVBUF_KB", "ENET_PEERCOUNT",
	"ENET_INITIAL_RTT_MS", "ENET_INITIAL_RTT_VAR_MS",
	"ENET_PING_MS", "ENET_TIMEOUT_LIMIT",
	"ENET_TIMEOUT_MIN_MS", "ENET_TIMEOUT_MAX_MS",
	"ENET_UNRELIABLE_MODE", "ENET_UNSEQUENCED",
	"RUDP_SERVER_RECV_DRAIN_LIMIT",
	"APEX_RCVBUF_KB", "APEX_ASYNC_SEND",
	"APEX_ASYNC_UNRELIABLE_SERVER", "APEX_ACK_DELAY_MS",
	"APEX_RX_WORKER", "APEX_SPLIT_ACK",
	"APEX_RECV_DRAIN_ON_EMPTY", "APEX_RECV_EMPTY_DRAINS",
}

type IsolateMode string

const (
	IsolateTaskset IsolateMode = "taskset"
	IsolateSystemd IsolateMode = "systemd"
)

func IsolateSetup() error {
	cmd := exec.Command("sudo", "bash", "-c", strings.Join([]string{
		`systemctl set-property -- system.slice AllowedCPUs=0-2,8-10`,
		`systemctl set-property -- user.slice   AllowedCPUs=0-2,8-10`,
		`systemctl set-property -- init.scope   AllowedCPUs=0-2,8-10`,
		`for f in /proc/irq/*/smp_affinity_list; do echo 0-2,8-10 > "$f" 2>/dev/null || true; done`,
		`for cpu in 3 4 5 6 7 11 12 13 14 15; do gov=/sys/devices/system/cpu/cpu${cpu}/cpufreq/scaling_governor; [ -e "$gov" ] && echo performance > "$gov" 2>/dev/null || true; done`,
		`cpupower -c 3,4,5,6,7,11,12,13,14,15 idle-set -D 1 >/dev/null 2>&1 || true`,
	}, " && "))
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	return cmd.Run()
}

func IsolateTeardown() error {
	cmd := exec.Command("sudo", "bash", "-c", strings.Join([]string{
		`systemctl set-property -- system.slice AllowedCPUs=`,
		`systemctl set-property -- user.slice   AllowedCPUs=`,
		`systemctl set-property -- init.scope   AllowedCPUs=`,
		`for f in /proc/irq/*/smp_affinity_list; do echo 0-15 > "$f" 2>/dev/null || true; done`,
		`cpupower -c 3,4,5,6,7,11,12,13,14,15 idle-set -E >/dev/null 2>&1 || true`,
	}, " && "))
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	return cmd.Run()
}

type RunOpts struct {
	CPU       string
	Timeout   time.Duration
	Role      string // "server" or "client"
	Isolate   IsolateMode
	Dir       string
	Stdout    *os.File
	Stderr    *os.File
}

func RunIsolated(ctx context.Context, opts RunOpts, name string, args ...string) (*exec.Cmd, error) {
	var cmd *exec.Cmd

	timeoutSec := int(opts.Timeout.Seconds())
	if timeoutSec < 1 {
		timeoutSec = 1
	}

	if opts.Isolate == IsolateSystemd && opts.CPU != "" {
		sysArgs := []string{
			"systemd-run",
			"--slice=bench-" + opts.Role + ".slice",
			"--working-directory=" + opts.Dir,
			"-p", "AllowedCPUs=" + opts.CPU,
			"-p", "CPUWeight=10000",
		}

		u, err := user.Current()
		if err == nil {
			sysArgs = append(sysArgs, "-p", "User="+u.Username)
		}
		sysArgs = append(sysArgs,
			"-p", "LimitCORE=infinity",
			"-p", "LimitNOFILE=524288",
			"-p", "RuntimeMaxSec="+strconv.Itoa(timeoutSec)+"s",
		)

		for _, key := range envPassthrough {
			if val, ok := os.LookupEnv(key); ok {
				sysArgs = append(sysArgs, "-E", key+"="+val)
			}
		}

		sysArgs = append(sysArgs, "--quiet", "--wait", "--pipe", "--collect")
		sysArgs = append(sysArgs, name)
		sysArgs = append(sysArgs, args...)

		outerArgs := append([]string{
			fmt.Sprintf("%ds", timeoutSec+5),
			"sudo",
		}, sysArgs...)
		cmd = exec.CommandContext(ctx, "timeout", outerArgs...)
	} else if opts.CPU != "" {
		outerArgs := append([]string{
			fmt.Sprintf("%ds", timeoutSec),
			"taskset", "-c", opts.CPU,
			name,
		}, args...)
		cmd = exec.CommandContext(ctx, "timeout", outerArgs...)
	} else {
		outerArgs := append([]string{
			fmt.Sprintf("%ds", timeoutSec),
			name,
		}, args...)
		cmd = exec.CommandContext(ctx, "timeout", outerArgs...)
	}

	cmd.Dir = opts.Dir
	cmd.Stdout = opts.Stdout
	cmd.Stderr = opts.Stderr
	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	// context cancel (Ctrl-C 等) 時、既定の SIGKILL は直下の timeout(1) にしか
	// 届かず孫の harness が孤児化する。Setpgid: true により pgid == 子の pid
	// なので、プロセスグループ全体へ SIGKILL を送って孫ごと確実に落とす。
	cmd.Cancel = func() error {
		if cmd.Process == nil {
			return os.ErrProcessDone
		}
		return syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL)
	}
	// Cancel 後に I/O パイプ等が残っても Wait が永久ブロックしないよう猶予を設定。
	cmd.WaitDelay = 5 * time.Second

	if err := cmd.Start(); err != nil {
		return nil, err
	}
	return cmd, nil
}
