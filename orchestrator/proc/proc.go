package proc

import (
	"context"
	"io"
	"os"
	"os/exec"
	"syscall"
	"time"
)

const ControlSockEnv = "BENCH_CONTROL_SOCK"

type Spec struct {
	Path        string
	Args        []string
	Env         []string
	Dir         string
	ControlSock string
	Stdout      io.Writer
	Stderr      io.Writer
	WaitDelay   time.Duration
}

func Start(ctx context.Context, spec Spec) (*exec.Cmd, error) {
	cmd := exec.CommandContext(ctx, spec.Path, spec.Args...)
	cmd.Dir = spec.Dir
	cmd.Stdout = spec.Stdout
	cmd.Stderr = spec.Stderr
	cmd.Env = append(os.Environ(), spec.Env...)
	if spec.ControlSock != "" {
		cmd.Env = append(cmd.Env, ControlSockEnv+"="+spec.ControlSock)
	}

	cmd.SysProcAttr = &syscall.SysProcAttr{Setpgid: true}
	cmd.Cancel = func() error {
		if cmd.Process == nil {
			return os.ErrProcessDone
		}
		if err := syscall.Kill(-cmd.Process.Pid, syscall.SIGKILL); err != nil {
			if err == syscall.ESRCH {
				return os.ErrProcessDone
			}
			return err
		}
		return nil
	}
	if spec.WaitDelay > 0 {
		cmd.WaitDelay = spec.WaitDelay
	} else {
		cmd.WaitDelay = 5 * time.Second
	}
	if err := cmd.Start(); err != nil {
		return nil, err
	}
	return cmd, nil
}
