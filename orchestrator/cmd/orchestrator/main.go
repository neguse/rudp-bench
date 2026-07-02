package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/control"
	"github.com/neguse/rudp-bench/orchestrator/netops"
)

func main() {
	var (
		sock       = flag.String("sock", filepath.Join(os.TempDir(), fmt.Sprintf("rudp-bench-%d.sock", os.Getpid())), "control Unix domain socket path")
		expected   = flag.Int("expected", 1, "expected process count")
		warmup     = flag.Duration("warmup", time.Second, "warmup before measurement window")
		duration   = flag.Duration("duration", time.Second, "measurement window duration")
		drain      = flag.Duration("drain", time.Second, "post-stop receive drain")
		timeout    = flag.Duration("timeout", 30*time.Second, "per-stage control timeout")
		netDryRun  = flag.Bool("netops-dry-run", false, "print default netns/veth/netem commands and exit")
		netPrefix  = flag.String("net-prefix", "rudpbench", "netns/veth name prefix for -netops-dry-run")
		serverLoss = flag.Float64("server-loss", 0, "server egress netem loss percent for -netops-dry-run")
		clientLoss = flag.Float64("client-loss", 0, "client egress netem loss percent for -netops-dry-run")
	)
	flag.Parse()

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	if *netDryRun {
		spec := netops.DefaultPair(*netPrefix)
		spec.ServerEgress = netops.Netem{DelayMS: 10, JitterMS: 1, LossPercent: *serverLoss, Limit: 10000}
		spec.ClientEgress = netops.Netem{DelayMS: 10, JitterMS: 1, LossPercent: *clientLoss, Limit: 10000}
		setup, err := netops.BuildSetupCommands(spec)
		exitOnErr(err)
		show, err := netops.BuildQdiscShowCommands(spec)
		exitOnErr(err)
		teardown, err := netops.BuildTeardownCommands(spec)
		exitOnErr(err)
		for _, group := range [][]netops.Command{setup, show, teardown} {
			exitOnErr(netops.RunCommands(ctx, group, netops.RunOptions{DryRun: true, Stdout: os.Stdout, Stderr: os.Stderr}))
		}
		return
	}

	cfg := control.Config{
		SocketPath:   *sock,
		Expected:     *expected,
		Warmup:       *warmup,
		Duration:     *duration,
		Drain:        *drain,
		HelloTimeout: *timeout,
		ReadyTimeout: *timeout,
		AckTimeout:   *timeout,
		DoneTimeout:  *timeout,
	}
	server, err := control.NewServer(cfg)
	exitOnErr(err)
	fmt.Fprintf(os.Stderr, "control socket: %s\n", server.SocketPath())
	result, err := server.Run(ctx)
	exitOnErr(err)
	enc := json.NewEncoder(os.Stdout)
	enc.SetIndent("", "  ")
	exitOnErr(enc.Encode(result))
}

func exitOnErr(err error) {
	if err == nil {
		return
	}
	fmt.Fprintln(os.Stderr, err)
	os.Exit(1)
}
