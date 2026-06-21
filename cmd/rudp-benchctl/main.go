package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"time"

	"github.com/neguse/rudp-bench/internal/bench"
	"github.com/neguse/rudp-bench/internal/cli"
	"github.com/neguse/rudp-bench/internal/sweep"
)

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
		os.Exit(1)
	}
}

func run(args []string) error {
	if len(args) == 0 || args[0] != "run" {
		fmt.Fprintln(os.Stderr, "usage: rudp-benchctl run [scenarios/file.json] [flags]")
		return errors.New("expected 'run' command")
	}
	args = args[1:]

	// Separate positional arg (scenario file) from flags so flags can appear
	// after the scenario file: `rudp-benchctl run scenarios/x.json --plan`
	// Only .json files are recognized as the scenario positional arg to avoid
	// misinterpreting flag values like `--profile echo`.
	var scenarioFile string
	var flagArgs []string
	for _, a := range args {
		if scenarioFile == "" && !strings.HasPrefix(a, "-") && strings.HasSuffix(a, ".json") {
			scenarioFile = a
		} else {
			flagArgs = append(flagArgs, a)
		}
	}

	root, err := cli.FindRepoRoot()
	if err != nil {
		return err
	}
	now := time.Now().UTC()
	defaultOut := filepath.Join(root, "results", "bench_"+now.Format("20060102T150405Z"))

	fs := flag.NewFlagSet("rudp-benchctl run", flag.ContinueOnError)

	// Output / build
	out := fs.String("out", cli.EnvDefault("OUT", defaultOut), "output directory")
	buildDir := fs.String("build-dir", cli.EnvDefault("BUILD_DIR", filepath.Join(root, "build")), "CMake build directory")
	jobs := fs.Int("jobs", cli.DefaultJobs(), "parallel build jobs")
	cmake := fs.String("cmake", cli.EnvDefault("CMAKE", "cmake"), "cmake executable")

	// Scenario override flags
	libs := fs.String("libs", "", "comma-separated libraries (overrides scenario)")
	lib := fs.String("lib", "", "single library (shorthand for --libs)")
	profiles := fs.String("profiles", "", "comma-separated profile names (overrides scenario)")
	profile := fs.String("profile", "", "single profile (shorthand for --profiles)")
	conns := fs.String("conns", "", "comma-separated conn counts (overrides scenario profile conns)")
	runs := fs.Int("runs", 0, "number of runs per point (overrides scenario)")
	duration := fs.Int("duration", 0, "measurement duration seconds (overrides scenario)")
	tailMs := fs.Int("tail-ms", 0, "tail collection milliseconds (overrides scenario)")
	netem := fs.Bool("netem", false, "enable netem")
	netemArgs := fs.String("netem-args", "", "netem args: 'delay_ms jitter_ms loss_pct limit_pkts'")
	isolate := fs.String("isolate", "", "isolation mode: taskset or systemd")
	serverCPU := fs.String("server-cpu", "", "server CPU pinning")
	clientCPU := fs.String("client-cpu", "", "client CPU pinning")
	minDelivery := fs.Float64("min-delivery", 0, "minimum delivery ratio gate")

	// Mode flags
	dryRun := fs.Bool("dry-run", cli.EnvBoolDefault("DRY_RUN", false), "print commands without executing")
	plan := fs.Bool("plan", false, "show execution plan and exit")
	resume := fs.Bool("resume", false, "resume from previous run")
	noBuild := fs.Bool("no-build", false, "skip cmake build")
	noPublish := fs.Bool("no-publish", false, "skip publish step")

	if err := fs.Parse(flagArgs); err != nil {
		return err
	}
	if fs.NArg() > 0 {
		return fmt.Errorf("unexpected args: %s", strings.Join(fs.Args(), " "))
	}

	// Determine mode: positional arg = scenario file, no positional = flags-only
	var scenario *bench.Scenario
	if scenarioFile != "" {
		scenarioPath := scenarioFile
		s, err := bench.LoadScenario(cli.AbsPath(root, scenarioPath))
		if err != nil {
			return fmt.Errorf("load scenario %s: %w", scenarioPath, err)
		}
		scenario = s

		if scenario.Locked {
			var overridden []string
			fs.Visit(func(f *flag.Flag) {
				switch f.Name {
				case "out", "build-dir", "jobs", "cmake", "dry-run", "plan", "resume", "no-build", "no-publish":
					// allowed
				default:
					overridden = append(overridden, "--"+f.Name)
				}
			})
			if len(overridden) > 0 {
				return fmt.Errorf("scenario is locked, cannot override: %s", strings.Join(overridden, ", "))
			}
		}
	} else {
		scenario = bench.DefaultScenario()
	}

	// Apply flag overrides to non-locked scenario
	if !scenario.Locked {
		if *lib != "" && *libs == "" {
			*libs = *lib
		}
		if *libs != "" {
			scenario.Libs = splitCSV(*libs)
		}
		if *profile != "" && *profiles == "" {
			*profiles = *profile
		}
		if *profiles != "" {
			scenario.Profiles = splitCSV(*profiles)
		}
		if *runs > 0 {
			scenario.Runs = *runs
		}
		if *duration > 0 {
			scenario.Duration = *duration
		}
		if *tailMs > 0 {
			scenario.TailMs = *tailMs
		}
		if *netem {
			scenario.Netem = true
		}
		if *netemArgs != "" {
			scenario.NetemArgs = *netemArgs
		}
		if *isolate != "" {
			scenario.Isolate = *isolate
		}
		if *serverCPU != "" {
			scenario.ServerCPU = *serverCPU
		}
		if *clientCPU != "" {
			scenario.ClientCPU = *clientCPU
		}
		if *minDelivery > 0 {
			scenario.MinDelivery = *minDelivery
		}
	}

	if *conns != "" && !scenario.Locked {
		connStr := strings.ReplaceAll(*conns, ",", " ")
		for _, pname := range scenario.Profiles {
			switch pname {
			case "media_relay":
				scenario.MediaConns = connStr
			case "game_server":
				scenario.GameConns = connStr
			case "reliable_echo":
				scenario.ReliableEchoConns = connStr
			case "echo":
				scenario.EchoConns = connStr
			}
		}
	}

	if len(scenario.Libs) == 0 {
		return errors.New("no libraries specified")
	}
	if len(scenario.Profiles) == 0 {
		return errors.New("no profiles specified")
	}

	// Build profile list
	profileObjs := make([]bench.Profile, 0, len(scenario.Profiles))
	for _, pname := range scenario.Profiles {
		p, ok := bench.ProfileByName(pname)
		if !ok {
			return fmt.Errorf("unknown profile: %s", pname)
		}
		// Apply per-profile conns override from scenario
		switch pname {
		case "media_relay":
			if scenario.MediaConns != "" {
				p.Conns = bench.ParseInts(scenario.MediaConns)
			}
		case "game_server":
			if scenario.GameConns != "" {
				p.Conns = bench.ParseInts(scenario.GameConns)
			}
		case "reliable_echo":
			if scenario.ReliableEchoConns != "" {
				p.Conns = bench.ParseInts(scenario.ReliableEchoConns)
			}
		case "echo":
			if scenario.EchoConns != "" {
				p.Conns = bench.ParseInts(scenario.EchoConns)
			}
		}
		profileObjs = append(profileObjs, p)
	}

	// Build run IDs
	runIDs := make([]string, 0, scenario.Runs)
	for i := 1; i <= scenario.Runs; i++ {
		runIDs = append(runIDs, fmt.Sprintf("%d", i))
	}

	minValid := scenario.MinValid
	if minValid <= 0 {
		minValid = scenario.Runs
		if minValid > 2 {
			minValid = 2
		}
	}

	cfg := sweep.RunConfig{
		Root:          root,
		Out:           cli.AbsPath(root, *out),
		BuildDir:      cli.AbsPath(root, *buildDir),
		Libs:          scenario.Libs,
		Profiles:      profileObjs,
		Runs:          runIDs,
		Duration:      scenario.Duration,
		TailMs:        scenario.TailMs,
		Idle:          "adaptive",
		Isolate:       scenario.Isolate,
		ServerCPU:     scenario.ServerCPU,
		ClientCPU:     scenario.ClientCPU,
		Netem:         scenario.Netem,
		NetemArgs:     scenario.NetemArgs,
		MinValid:      minValid,
		MinDelivery:   scenario.MinDelivery,
		Build:         scenario.Build,
		Publish:       scenario.Publish,
		NoBuild:       *noBuild,
		NoPublish:     *noPublish,
		DryRun:        *dryRun,
		Plan:          *plan,
		Resume:        *resume,
		Jobs:          *jobs,
		CMake:         *cmake,
		LitenetlibBin: "adapters/litenetlib/bin/Release/net10.0/litenetlib_adapter",
	}

	if cfg.Plan {
		sweep.ShowPlan(cfg)
		return nil
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt)
	defer stop()

	return sweep.Run(ctx, cfg)
}

func splitCSV(s string) []string {
	var out []string
	for _, item := range strings.Split(s, ",") {
		item = strings.TrimSpace(item)
		if item != "" {
			out = append(out, item)
		}
	}
	return out
}
