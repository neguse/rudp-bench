package main

import (
	"context"
	"encoding/json"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/aggregate"
	"github.com/neguse/rudp-bench/orchestrator/block"
	"github.com/neguse/rudp-bench/orchestrator/boundary"
	"github.com/neguse/rudp-bench/orchestrator/control"
	"github.com/neguse/rudp-bench/orchestrator/netops"
	"github.com/neguse/rudp-bench/orchestrator/report"
	"github.com/neguse/rudp-bench/orchestrator/sentinel"
	orun "github.com/neguse/rudp-bench/orchestrator/run"
	"github.com/neguse/rudp-bench/orchestrator/sweep"
)

func main() {
	if len(os.Args) > 1 && os.Args[1] == "run" {
		runMain(os.Args[2:])
		return
	}
	if len(os.Args) > 1 && os.Args[1] == "sweep" {
		sweepMain(os.Args[2:])
		return
	}
	if len(os.Args) > 1 && os.Args[1] == "report" {
		reportMain(os.Args[2:])
		return
	}
	if len(os.Args) > 1 && os.Args[1] == "boundary" {
		boundaryMain(os.Args[2:])
		return
	}
	if len(os.Args) > 1 && os.Args[1] == "sentinel" {
		fs := flag.NewFlagSet("sentinel", flag.ExitOnError)
		configPath := fs.String("config", "", "sentinel config JSON path")
		exitOnErr(fs.Parse(os.Args[2:]))
		if *configPath == "" {
			fmt.Fprintln(os.Stderr, "sentinel -config is required")
			os.Exit(1)
		}
		cfg, err := sentinel.LoadConfig(*configPath)
		exitOnErr(err)
		ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
		defer stop()
		drift, invalid, err := sentinel.Run(ctx, cfg)
		exitOnErr(err)
		// invalid(測定不成立)は drift(漂移)と別の終了コード。環境不成立の
		// 全滅を「全点漂移」として検知させない。invalid が1つでもあれば
		// 結果自体が信用できないので rc=4 を優先する
		if invalid > 0 {
			fmt.Fprintf(os.Stderr, "sentinel: %d probe(s) INVALID (measurement failed), %d drifted\n", invalid, drift)
			os.Exit(4)
		}
		if drift > 0 {
			fmt.Fprintf(os.Stderr, "sentinel: %d probe(s) drifted\n", drift)
			os.Exit(3)
		}
		return
	}
	if len(os.Args) > 1 && os.Args[1] == "aggregate" {
		fs := flag.NewFlagSet("aggregate", flag.ExitOnError)
		var sweeps, boundaries repeatedFlag
		fs.Var(&sweeps, "sweep", "same-regime sweep dir from each block (repeatable)")
		fs.Var(&boundaries, "boundary", "boundary dir from each block (repeatable)")
		anchorsOnly := fs.Bool("anchors-only", false, "capacity table restricted to anchor cells")
		fs.String("doc", "", "write tables into <!-- generated:… --> markers of this markdown instead of stdout")
		exitOnErr(fs.Parse(os.Args[2:]))
		if len(sweeps) == 0 && len(boundaries) == 0 {
			fmt.Fprintln(os.Stderr, "aggregate needs -sweep and/or -boundary (one per block)")
			os.Exit(1)
		}
		doc := fs.Lookup("doc").Value.String()
		sections := map[string]string{}
		if len(sweeps) > 0 {
			aggs, regime, err := aggregate.AggregateCapacityWithRegime(sweeps)
			exitOnErr(err)
			table := aggregate.CapacityCITable(aggs, regime, *anchorsOnly)
			if doc == "" {
				fmt.Println(table)
			} else {
				sections["capacity-ci-"+regime] = table
			}
		}
		if len(boundaries) > 0 {
			aggs, err := aggregate.AggregateBoundary(boundaries)
			exitOnErr(err)
			for _, anchor := range aggregate.BoundaryAnchors(aggs) {
				for _, label := range aggregate.BoundaryLoadLabels(aggs, anchor) {
					table := aggregate.BoundaryCITable(aggs, anchor, label)
					if doc == "" {
						fmt.Printf("### %s / %s\n\n%s\n", anchor, label, table)
					} else {
						sections["boundary-ci-"+anchor+"-"+label] = table
					}
				}
			}
		}
		if doc != "" {
			exitOnErr(report.UpdateSections(doc, sections))
			fmt.Fprintf(os.Stderr, "updated: %s\n", doc)
		}
		return
	}
	if len(os.Args) > 1 && os.Args[1] == "block" {
		fs := flag.NewFlagSet("block", flag.ExitOnError)
		configPath := fs.String("config", "", "block config JSON path")
		exitOnErr(fs.Parse(os.Args[2:]))
		if *configPath == "" {
			fmt.Fprintln(os.Stderr, "block -config is required")
			os.Exit(1)
		}
		cfg, err := block.LoadConfig(*configPath)
		exitOnErr(err)
		ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
		defer stop()
		exitOnErr(block.Run(ctx, cfg))
		return
	}
	if len(os.Args) > 1 && os.Args[1] == "losstrace" {
		losstraceMain(os.Args[2:])
		return
	}
	if len(os.Args) > 1 && os.Args[1] == "isolate" {
		isolateMain(os.Args[2:])
		return
	}
	if len(os.Args) > 1 && os.Args[1] == "rejudge" {
		fs := flag.NewFlagSet("rejudge", flag.ExitOnError)
		dir := fs.String("dir", "", "sweep output dir to re-judge from stored run results")
		schedM := fs.String("sched-measurand", "", "comma-separated transports whose sched latency is the measurand (TCP family)")
		exitOnErr(fs.Parse(os.Args[2:]))
		if *dir == "" {
			fmt.Fprintln(os.Stderr, "rejudge -dir is required")
			os.Exit(1)
		}
		override := map[string]bool{}
		for _, t := range strings.Split(*schedM, ",") {
			if t != "" {
				override[t] = true
			}
		}
		exitOnErr(sweep.Rejudge(*dir, override))
		return
	}

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

func runMain(args []string) {
	fs := flag.NewFlagSet("run", flag.ExitOnError)
	configPath := fs.String("config", "", "run config JSON path")
	exitOnErr(fs.Parse(args))
	if *configPath == "" {
		fmt.Fprintln(os.Stderr, "run -config is required")
		os.Exit(1)
	}

	cfg, err := orun.LoadConfig(*configPath)
	exitOnErr(err)

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	result, err := orun.Run(ctx, cfg)
	exitOnErr(err)
	if result != nil {
		if path := result.Artifacts["result_json"]; path != "" {
			fmt.Fprintf(os.Stderr, "result: %s\n", path)
		}
		if path := result.Artifacts["summary"]; path != "" {
			fmt.Fprintf(os.Stderr, "summary: %s\n", path)
		}
		if result.Verdict != orun.VerdictValid {
			os.Exit(2)
		}
	}
}

type repeatedFlag []string

func (r *repeatedFlag) String() string     { return strings.Join(*r, ",") }
func (r *repeatedFlag) Set(v string) error { *r = append(*r, v); return nil }

func reportMain(args []string) {
	fs := flag.NewFlagSet("report", flag.ExitOnError)
	doc := fs.String("doc", "docs/measurements/v2-draft/report.md", "markdown doc with generated markers")
	var sweeps, boundaries repeatedFlag
	fs.Var(&sweeps, "sweep", "capacity sweep output dir (repeatable)")
	fs.Var(&boundaries, "boundary", "boundary sweep output dir (repeatable)")
	exitOnErr(fs.Parse(args))
	if len(sweeps) == 0 && len(boundaries) == 0 {
		fmt.Fprintln(os.Stderr, "report needs -sweep and/or -boundary")
		os.Exit(1)
	}
	exitOnErr(report.UpdateDoc(*doc, sweeps, boundaries))
	fmt.Fprintf(os.Stderr, "updated: %s\n", *doc)
}

func boundaryMain(args []string) {
	fs := flag.NewFlagSet("boundary", flag.ExitOnError)
	configPath := fs.String("config", "", "boundary config JSON path")
	exitOnErr(fs.Parse(args))
	if *configPath == "" {
		fmt.Fprintln(os.Stderr, "boundary -config is required")
		os.Exit(1)
	}
	cfg, err := boundary.LoadConfig(*configPath)
	exitOnErr(err)

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	b, err := boundary.New(cfg)
	exitOnErr(err)
	defer b.Close()

	points, err := b.Run(ctx)
	if len(points) > 0 {
		fmt.Fprintf(os.Stderr, "boundary: %s\n", filepath.Join(cfg.OutputDir, "boundary.json"))
	}
	exitOnErr(err)
}

func sweepMain(args []string) {
	fs := flag.NewFlagSet("sweep", flag.ExitOnError)
	configPath := fs.String("config", "", "sweep config JSON path")
	exitOnErr(fs.Parse(args))
	if *configPath == "" {
		fmt.Fprintln(os.Stderr, "sweep -config is required")
		os.Exit(1)
	}

	cfg, err := sweep.LoadConfig(*configPath)
	exitOnErr(err)

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	s, err := sweep.New(cfg)
	exitOnErr(err)
	defer s.Close()

	cells, err := s.Run(ctx)
	if len(cells) > 0 {
		fmt.Fprintf(os.Stderr, "capacity: %s\n", filepath.Join(cfg.OutputDir, "capacity.json"))
	}
	exitOnErr(err)
}

func exitOnErr(err error) {
	if err == nil {
		return
	}
	fmt.Fprintln(os.Stderr, err)
	os.Exit(1)
}
