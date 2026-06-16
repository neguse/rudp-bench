package main

import (
	"errors"
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"time"
)

const (
	canonicalLibs              = "mini_rudp,coop_rudp,apex_rudp,enet,kcp,slikenet,raknet,udt4,yojimbo,gns,litenetlib,msquic"
	canonicalRuns              = "1 2 3"
	canonicalNetemArgs         = "25 5 1 100000"
	canonicalMediaConns        = "1 5 50 75 100 125 150 200"
	canonicalGameConns         = "1 5 64 96 128 192 256"
	canonicalEchoConns         = "1 50 200 600 1000 1500 2000 3000"
	canonicalReliableEchoConns = "1 50 200 600 1000 1500 2000 3000"
)

type config struct {
	root             string
	cmake            string
	python           string
	buildDir         string
	out              string
	publishID        string
	jobs             int
	updateSubmodules bool
	publishDocs      bool
	benchIsolate     bool
	dryRun           bool
}

type commandRunner struct {
	dir    string
	dryRun bool
}

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
		os.Exit(1)
	}
}

func run(args []string) (err error) {
	cfg, err := parseConfig(args)
	if err != nil {
		return err
	}
	r := commandRunner{dir: cfg.root, dryRun: cfg.dryRun}

	fmt.Println("==> canonical benchmark")
	fmt.Println("==> repo:", cfg.root)
	fmt.Println("==> build dir:", cfg.buildDir)
	fmt.Println("==> output dir:", cfg.out)
	fmt.Println("==> libraries:", canonicalLibs)
	fmt.Println("==> runs:", canonicalRuns)

	if cfg.updateSubmodules {
		if err := r.run("git", "-C", cfg.root, "submodule", "update", "--init", "--recursive"); err != nil {
			return err
		}
	}

	if err := r.run(cfg.cmake, "-S", cfg.root, "-B", cfg.buildDir); err != nil {
		return err
	}
	if err := r.run(cfg.cmake, "--build", cfg.buildDir, "-j", strconv.Itoa(cfg.jobs)); err != nil {
		return err
	}

	benchIsolateActive := false
	defer func() {
		if benchIsolateActive {
			if teardownErr := r.run(filepath.Join(cfg.root, "scripts/bench_isolate.sh"), "teardown"); err == nil {
				err = teardownErr
			}
		}
	}()

	if cfg.benchIsolate {
		if err := r.run(filepath.Join(cfg.root, "scripts/bench_isolate.sh"), "setup"); err != nil {
			return err
		}
		if !cfg.dryRun {
			benchIsolateActive = true
		}
	}

	if err := r.run(
		cfg.python,
		"scripts/run_final_saturation_profiles.py",
		"--out", cfg.out,
		"--build-dir", cfg.buildDir,
		"--libraries", canonicalLibs,
		"--runs", canonicalRuns,
		"--netem", "1",
		"--netem-args", canonicalNetemArgs,
		"--duration", "20",
		"--tail-ms", "500",
		"--idle", "adaptive",
		"--isolate", "systemd",
		"--server-cpu", "7,15",
		"--client-cpu", "3,4,5,6,11,12,13,14",
		"--min-valid", "2",
		"--min-delivery", "0.95",
		"--media-conns", canonicalMediaConns,
		"--game-conns", canonicalGameConns,
		"--echo-conns", canonicalEchoConns,
		"--reliable-echo-conns", canonicalReliableEchoConns,
		"--broadcast-client-procs", "4",
		"--echo-client-procs", "8",
	); err != nil {
		return err
	}

	if err := r.run(cfg.python, "scripts/render_canonical_report.py", "--run-dir", cfg.out); err != nil {
		return err
	}

	if cfg.publishDocs {
		if err := r.run(
			cfg.python,
			"scripts/publish_canonical_result.py",
			"--run-dir", cfg.out,
			"--dest", filepath.Join(cfg.root, "docs/measurements", cfg.publishID),
			"--current", filepath.Join(cfg.root, "docs/measurements/current.md"),
		); err != nil {
			return err
		}
	}

	if !cfg.dryRun {
		fmt.Println("==> canonical benchmark complete")
		fmt.Println("==> report:", filepath.Join(cfg.out, "report.md"))
		if cfg.publishDocs {
			fmt.Println("==> published docs report:", filepath.Join(cfg.root, "docs/measurements", cfg.publishID, "report.md"))
			fmt.Println("==> current docs pointer:", filepath.Join(cfg.root, "docs/measurements/current.md"))
		}
		fmt.Println("==> capacity:", filepath.Join(cfg.out, "capacity.csv"))
		fmt.Println("==> summary:", filepath.Join(cfg.out, "summary.csv"))
	}
	return nil
}

func parseConfig(args []string) (config, error) {
	root, err := findRepoRoot()
	if err != nil {
		return config{}, err
	}
	now := time.Now().UTC()
	defaultOut := filepath.Join(root, "results", "canonical_final_benchmark_"+now.Format("20060102T150405Z"))
	defaultPublishID := now.Format("2006-01-02-canonical-150405Z")

	cfg := config{
		root:             root,
		cmake:            envDefault("CMAKE", "cmake"),
		python:           envDefault("PYTHON", "python3"),
		out:              envDefault("OUT", defaultOut),
		buildDir:         envDefault("BUILD_DIR", filepath.Join(root, "build")),
		publishID:        envDefault("PUBLISH_ID", defaultPublishID),
		jobs:             defaultJobs(),
		updateSubmodules: envBoolDefault("UPDATE_SUBMODULES", true),
		publishDocs:      envBoolDefault("PUBLISH_DOCS", true),
		benchIsolate:     envBoolDefault("BENCH_ISOLATE", true),
		dryRun:           envBoolDefault("DRY_RUN", false),
	}

	fs := flag.NewFlagSet("rudp-bench-canonical", flag.ContinueOnError)
	fs.StringVar(&cfg.out, "out", cfg.out, "output directory")
	fs.StringVar(&cfg.buildDir, "build-dir", cfg.buildDir, "CMake build directory")
	fs.StringVar(&cfg.publishID, "publish-id", cfg.publishID, "dated docs/measurements id")
	fs.IntVar(&cfg.jobs, "jobs", cfg.jobs, "parallel build jobs")
	fs.StringVar(&cfg.cmake, "cmake", cfg.cmake, "cmake executable")
	fs.StringVar(&cfg.python, "python", cfg.python, "python executable")
	noSubmoduleUpdate := fs.Bool("no-submodule-update", !cfg.updateSubmodules, "skip git submodule update")
	noPublishDocs := fs.Bool("no-publish-docs", !cfg.publishDocs, "do not publish docs/measurements output")
	noBenchIsolate := fs.Bool("no-bench-isolate", !cfg.benchIsolate, "do not run scripts/bench_isolate.sh setup/teardown")
	fs.BoolVar(&cfg.dryRun, "dry-run", cfg.dryRun, "print commands without executing them")
	if err := fs.Parse(args); err != nil {
		return config{}, err
	}
	if fs.NArg() != 0 {
		return config{}, fmt.Errorf("unexpected args: %s", strings.Join(fs.Args(), " "))
	}
	cfg.updateSubmodules = !*noSubmoduleUpdate
	cfg.publishDocs = !*noPublishDocs
	cfg.benchIsolate = !*noBenchIsolate
	if cfg.jobs < 1 {
		return config{}, errors.New("--jobs must be >= 1")
	}
	cfg.out = absPath(root, cfg.out)
	cfg.buildDir = absPath(root, cfg.buildDir)
	return cfg, nil
}

func (r commandRunner) run(name string, args ...string) error {
	argv := append([]string{name}, args...)
	fmt.Println("+", quoteCommand(argv))
	if r.dryRun {
		return nil
	}
	cmd := exec.Command(name, args...)
	cmd.Dir = r.dir
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	cmd.Stdin = os.Stdin
	return cmd.Run()
}

func findRepoRoot() (string, error) {
	wd, err := os.Getwd()
	if err != nil {
		return "", err
	}
	dir, err := filepath.Abs(wd)
	if err != nil {
		return "", err
	}
	for {
		if fileExists(filepath.Join(dir, "CMakeLists.txt")) && fileExists(filepath.Join(dir, "scripts", "run_final_saturation_profiles.py")) {
			return dir, nil
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return "", errors.New("could not find repository root")
		}
		dir = parent
	}
}

func fileExists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}

func envDefault(key, fallback string) string {
	if value := os.Getenv(key); value != "" {
		return value
	}
	return fallback
}

func envBoolDefault(key string, fallback bool) bool {
	value := os.Getenv(key)
	if value == "" {
		return fallback
	}
	switch strings.ToLower(value) {
	case "1", "true", "yes", "on":
		return true
	case "0", "false", "no", "off":
		return false
	default:
		return fallback
	}
}

func defaultJobs() int {
	if value := os.Getenv("JOBS"); value != "" {
		if parsed, err := strconv.Atoi(value); err == nil && parsed > 0 {
			return parsed
		}
	}
	return runtime.NumCPU()
}

func absPath(root, path string) string {
	if filepath.IsAbs(path) {
		return filepath.Clean(path)
	}
	return filepath.Join(root, path)
}

func quoteCommand(argv []string) string {
	parts := make([]string, 0, len(argv))
	for _, arg := range argv {
		parts = append(parts, quoteArg(arg))
	}
	return strings.Join(parts, " ")
}

func quoteArg(arg string) string {
	if arg == "" {
		return "''"
	}
	if strings.IndexFunc(arg, func(r rune) bool {
		return !(r >= 'A' && r <= 'Z' ||
			r >= 'a' && r <= 'z' ||
			r >= '0' && r <= '9' ||
			strings.ContainsRune("@%_+=:,./-", r))
	}) == -1 {
		return arg
	}
	return "'" + strings.ReplaceAll(arg, "'", `'\''`) + "'"
}
