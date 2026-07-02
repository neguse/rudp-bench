package proc_test

import (
	"bytes"
	"context"
	"os"
	"syscall"
	"testing"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/proc"
)

func TestStartInjectsControlSockAndSetsProcessGroup(t *testing.T) {
	if _, err := os.Stat("/bin/sh"); err != nil {
		t.Skip("/bin/sh is not available")
	}
	const sock = "/tmp/rudp-bench-control.sock"
	var stdout bytes.Buffer
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()

	cmd, err := proc.Start(ctx, proc.Spec{
		Path:        "/bin/sh",
		Args:        []string{"-c", `printf '%s' "$BENCH_CONTROL_SOCK"; sleep 0.05`},
		ControlSock: sock,
		Stdout:      &stdout,
	})
	if err != nil {
		t.Fatal(err)
	}
	pgid, err := syscall.Getpgid(cmd.Process.Pid)
	if err != nil {
		t.Fatal(err)
	}
	if pgid != cmd.Process.Pid {
		t.Fatalf("pgid = %d, want child pid %d", pgid, cmd.Process.Pid)
	}
	if err := cmd.Wait(); err != nil {
		t.Fatal(err)
	}
	if stdout.String() != sock {
		t.Fatalf("%s = %q, want %q", proc.ControlSockEnv, stdout.String(), sock)
	}
}
