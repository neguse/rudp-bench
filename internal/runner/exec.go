package runner

import (
	"context"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/neguse/rudp-bench/internal/result"
)

// ExecOpts holds all parameters for running a single benchmark scenario.
type ExecOpts struct {
	BuildDir      string
	Library       string
	RateR, RateU  int
	Size, Conns   int
	Duration      int
	Warmup        int
	RampUpMs      int
	TailMs        int
	Loss          int
	Mode          string // "echo" or "broadcast"
	Idle          string // "spin" or "adaptive"
	ClientProcs   int
	Isolate       IsolateMode
	ServerCPU     string
	ClientCPU     string
	Port          int
	OutDir        string // directory for raw files
	RunID         string
	ScenarioID    string
	LitenetlibBin string
	DryRun        bool
	// Paths for reduce output
	Results     string
	Diagnostics string
	Scenarios   string
}

// DefaultPort returns the default port for a library given its index in the list.
func DefaultPort(libIndex int) int {
	return 30000 + libIndex + 1
}

// exitCode extracts the exit code from a completed *exec.Cmd.
// Returns 124 if the process was killed by context timeout, matching
// the timeout(1) convention used in the shell script.
func exitCode(cmd *exec.Cmd) int {
	if cmd == nil || cmd.ProcessState == nil {
		return -1
	}
	return cmd.ProcessState.ExitCode()
}

// Exec runs a single benchmark scenario (one library, one conn count, one
// profile point). It ports the core loop body from scripts/run_phase1_quick.sh.
func Exec(ctx context.Context, opts ExecOpts) error {
	// --- Resolve defaults ---
	rampUpMs := opts.RampUpMs
	if rampUpMs < 0 {
		if opts.Library == "msquic" {
			rampUpMs = 10000
		} else {
			rampUpMs = 0
		}
	}

	warmup := opts.Warmup
	if opts.Library == "litenetlib" && warmup < 5 {
		warmup = 5
	}

	// --- Binary selection ---
	var bin string
	if opts.Library == "litenetlib" {
		bin = opts.LitenetlibBin
		if bin == "" {
			bin = "adapters/litenetlib/bin/Release/net10.0/litenetlib_adapter"
		}
	} else {
		bin = filepath.Join(opts.BuildDir, "harness", "rudp-bench")
	}

	// --- Timeout calculation ---
	tailTimeoutS := (opts.TailMs + 999) / 1000
	rampTimeoutS := (rampUpMs + 999) / 1000
	connectTimeoutS := opts.Conns / 10
	timeoutS := opts.Duration + warmup + rampTimeoutS + tailTimeoutS + 10 + connectTimeoutS
	timeout := time.Duration(timeoutS) * time.Second

	// --- Raw file paths ---
	sid := opts.ScenarioID
	sOut := filepath.Join(opts.OutDir, "s_"+sid+".csv")
	cOut := filepath.Join(opts.OutDir, "c_"+sid+".csv")
	sStdout := filepath.Join(opts.OutDir, "s_"+sid+".stdout.log")
	sStderr := filepath.Join(opts.OutDir, "s_"+sid+".stderr.log")
	cStdout := filepath.Join(opts.OutDir, "c_"+sid+".stdout.log")
	cStderr := filepath.Join(opts.OutDir, "c_"+sid+".stderr.log")

	// Ensure output directory exists.
	if err := os.MkdirAll(opts.OutDir, 0o755); err != nil {
		return fmt.Errorf("mkdir %s: %w", opts.OutDir, err)
	}

	// --- Common args ---
	port := strconv.Itoa(opts.Port)
	trafficArgs := []string{
		"--rate-r=" + strconv.Itoa(opts.RateR),
		"--rate-u=" + strconv.Itoa(opts.RateU),
		"--duration=" + strconv.Itoa(opts.Duration),
		"--warmup=" + strconv.Itoa(warmup),
		"--ramp-up-ms=" + strconv.Itoa(rampUpMs),
		"--tail-ms=" + strconv.Itoa(opts.TailMs),
		"--loss=" + strconv.Itoa(opts.Loss),
		"--size=" + strconv.Itoa(opts.Size),
		"--conns=" + strconv.Itoa(opts.Conns),
		"--mode=" + opts.Mode,
		"--idle=" + opts.Idle,
	}

	serverArgs := append([]string{
		"--library=" + opts.Library,
		"--role=server",
		"--port=" + port,
	}, trafficArgs...)
	serverArgs = append(serverArgs, "--out="+sOut)

	// --- DryRun ---
	if opts.DryRun {
		fmt.Printf("[dry-run] server: %s %s\n", bin, strings.Join(serverArgs, " "))
		clientArgs := append([]string{
			"--library=" + opts.Library,
			"--role=client",
			"--host=127.0.0.1",
			"--port=" + port,
		}, trafficArgs...)
		clientArgs = append(clientArgs, "--out="+cOut)
		fmt.Printf("[dry-run] client: %s %s\n", bin, strings.Join(clientArgs, " "))
		return nil
	}

	// --- Check binary exists ---
	if _, err := os.Stat(bin); os.IsNotExist(err) {
		// Write stub log files and record missing binary status.
		writeEmpty(sStdout)
		writeEmpty(cStdout)
		msg := fmt.Sprintf("%s binary not found: %s\n", opts.Library, bin)
		os.WriteFile(sStderr, []byte(msg), 0o644)
		os.WriteFile(cStderr, []byte(msg), 0o644)

		return result.Append(result.AppendOpts{
			Results:      opts.Results,
			Diagnostics:  opts.Diagnostics,
			Scenarios:    opts.Scenarios,
			ServerCSV:    sOut,
			ClientCSV:    cOut,
			ServerStdout: sStdout,
			ServerStderr: sStderr,
			ClientStdout: cStdout,
			ClientStderr: cStderr,
			ServerStatus: "127",
			ClientStatus: "127",
			RunID:        opts.RunID,
			ScenarioID:   opts.ScenarioID,
			Library:      opts.Library,
			RateR:        strconv.Itoa(opts.RateR),
			RateU:        strconv.Itoa(opts.RateU),
			Size:         strconv.Itoa(opts.Size),
			Conns:        strconv.Itoa(opts.Conns),
			Loss:         strconv.Itoa(opts.Loss),
			Mode:         opts.Mode,
			Duration:     strconv.Itoa(opts.Duration),
			Warmup:       strconv.Itoa(warmup),
			RampUpMs:     strconv.Itoa(rampUpMs),
			TailMs:       strconv.Itoa(opts.TailMs),
			Idle:         opts.Idle,
			ServerCPUPin: opts.ServerCPU,
			ClientCPUPin: opts.ClientCPU,
		})
	}

	// --- Working directory for RunIsolated ---
	cwd, err := os.Getwd()
	if err != nil {
		return fmt.Errorf("getwd: %w", err)
	}

	// --- Start server ---
	sStdoutF, err := os.Create(sStdout)
	if err != nil {
		return fmt.Errorf("create %s: %w", sStdout, err)
	}
	sStderrF, err := os.Create(sStderr)
	if err != nil {
		sStdoutF.Close()
		return fmt.Errorf("create %s: %w", sStderr, err)
	}

	serverCmd, err := RunIsolated(ctx, RunOpts{
		CPU:     opts.ServerCPU,
		Timeout: timeout,
		Role:    "server",
		Isolate: opts.Isolate,
		Dir:     cwd,
		Stdout:  sStdoutF,
		Stderr:  sStderrF,
	}, bin, serverArgs...)
	if err != nil {
		sStdoutF.Close()
		sStderrF.Close()
		return fmt.Errorf("start server: %w", err)
	}

	// Sleep after server start: 0.5s for litenetlib, 0.2s for others.
	if opts.Library == "litenetlib" {
		time.Sleep(500 * time.Millisecond)
	} else {
		time.Sleep(200 * time.Millisecond)
	}

	// --- Run client(s) ---
	var clientStatus int

	if opts.ClientProcs <= 1 {
		// Single-process client.
		clientArgs := append([]string{
			"--library=" + opts.Library,
			"--role=client",
			"--host=127.0.0.1",
			"--port=" + port,
		}, trafficArgs...)
		clientArgs = append(clientArgs, "--out="+cOut)

		cStdoutF, err := os.Create(cStdout)
		if err != nil {
			sStdoutF.Close()
			sStderrF.Close()
			return fmt.Errorf("create %s: %w", cStdout, err)
		}
		cStderrF, err := os.Create(cStderr)
		if err != nil {
			sStdoutF.Close()
			sStderrF.Close()
			cStdoutF.Close()
			return fmt.Errorf("create %s: %w", cStderr, err)
		}

		clientCmd, err := RunIsolated(ctx, RunOpts{
			CPU:     opts.ClientCPU,
			Timeout: timeout,
			Role:    "client",
			Isolate: opts.Isolate,
			Dir:     cwd,
			Stdout:  cStdoutF,
			Stderr:  cStderrF,
		}, bin, clientArgs...)
		if err != nil {
			cStdoutF.Close()
			cStderrF.Close()
			sStdoutF.Close()
			sStderrF.Close()
			// Still wait for server.
			serverCmd.Wait()
			return fmt.Errorf("start client: %w", err)
		}

		clientCmd.Wait()
		clientStatus = exitCode(clientCmd)
		cStdoutF.Close()
		cStderrF.Close()
	} else {
		// Multi-process client farm.
		clientStatus = runMultiClient(ctx, opts, bin, port, trafficArgs, timeout, cwd, cOut, cStdout, cStderr)
	}

	// --- Wait for server ---
	serverCmd.Wait()
	serverStatus := exitCode(serverCmd)
	sStdoutF.Close()
	sStderrF.Close()

	// --- Reduce ---
	return result.Append(result.AppendOpts{
		Results:      opts.Results,
		Diagnostics:  opts.Diagnostics,
		Scenarios:    opts.Scenarios,
		ServerCSV:    sOut,
		ClientCSV:    cOut,
		ServerStdout: sStdout,
		ServerStderr: sStderr,
		ClientStdout: cStdout,
		ClientStderr: cStderr,
		ServerStatus: strconv.Itoa(serverStatus),
		ClientStatus: strconv.Itoa(clientStatus),
		RunID:        opts.RunID,
		ScenarioID:   opts.ScenarioID,
		Library:      opts.Library,
		RateR:        strconv.Itoa(opts.RateR),
		RateU:        strconv.Itoa(opts.RateU),
		Size:         strconv.Itoa(opts.Size),
		Conns:        strconv.Itoa(opts.Conns),
		Loss:         strconv.Itoa(opts.Loss),
		Mode:         opts.Mode,
		Duration:     strconv.Itoa(opts.Duration),
		Warmup:       strconv.Itoa(warmup),
		RampUpMs:     strconv.Itoa(rampUpMs),
		TailMs:       strconv.Itoa(opts.TailMs),
		Idle:         opts.Idle,
		ServerCPUPin: opts.ServerCPU,
		ClientCPUPin: opts.ClientCPU,
	})
}

// runMultiClient spawns N client processes, each handling a shard of connections,
// waits for all of them, then combines their output into the canonical client
// CSV and log files. Returns the first non-zero exit status, or 0 if all OK.
func runMultiClient(
	ctx context.Context,
	opts ExecOpts,
	bin, port string,
	trafficArgs []string,
	timeout time.Duration,
	cwd string,
	cOut, cStdout, cStderr string,
) int {
	n := opts.ClientProcs

	type procResult struct {
		status int
		err    error
	}

	results := make([]procResult, n)
	clientCSVs := make([]string, n)
	binsR := make([]string, n)
	binsU := make([]string, n)
	procStdouts := make([]string, n)
	procStderrs := make([]string, n)

	var wg sync.WaitGroup
	connOffset := 0

	for i := 0; i < n; i++ {
		connsI := opts.Conns / n
		if i < opts.Conns%n {
			connsI++
		}

		suffix := fmt.Sprintf("_p%d", i)
		cOutI := filepath.Join(opts.OutDir, "c_"+opts.ScenarioID+suffix+".csv")
		binsRI := filepath.Join(opts.OutDir, "c_"+opts.ScenarioID+suffix+"_r.bin")
		binsUI := filepath.Join(opts.OutDir, "c_"+opts.ScenarioID+suffix+"_u.bin")
		cStdoutI := filepath.Join(opts.OutDir, "c_"+opts.ScenarioID+suffix+".stdout.log")
		cStderrI := filepath.Join(opts.OutDir, "c_"+opts.ScenarioID+suffix+".stderr.log")

		clientCSVs[i] = cOutI
		binsR[i] = binsRI
		binsU[i] = binsUI
		procStdouts[i] = cStdoutI
		procStderrs[i] = cStderrI

		clientArgs := append([]string{
			"--library=" + opts.Library,
			"--role=client",
			"--host=127.0.0.1",
			"--port=" + port,
		}, trafficArgs...)
		// Override --conns with the per-proc shard.
		clientArgs = replaceArg(clientArgs, "--conns=", "--conns="+strconv.Itoa(connsI))
		clientArgs = append(clientArgs,
			"--fanout-conns="+strconv.Itoa(opts.Conns),
			"--conn-id-offset="+strconv.Itoa(connOffset),
			"--out="+cOutI,
			"--bins-r-out="+binsRI,
			"--bins-u-out="+binsUI,
		)

		connOffset += connsI

		wg.Add(1)
		go func(idx int, args []string, stdoutPath, stderrPath string) {
			defer wg.Done()

			outF, err := os.Create(stdoutPath)
			if err != nil {
				results[idx] = procResult{status: -1, err: err}
				return
			}
			errF, err := os.Create(stderrPath)
			if err != nil {
				outF.Close()
				results[idx] = procResult{status: -1, err: err}
				return
			}

			cmd, err := RunIsolated(ctx, RunOpts{
				CPU:     opts.ClientCPU,
				Timeout: timeout,
				Role:    "client",
				Isolate: opts.Isolate,
				Dir:     cwd,
				Stdout:  outF,
				Stderr:  errF,
			}, bin, args...)
			if err != nil {
				outF.Close()
				errF.Close()
				results[idx] = procResult{status: -1, err: err}
				return
			}

			cmd.Wait()
			outF.Close()
			errF.Close()
			results[idx] = procResult{status: exitCode(cmd)}
		}(i, clientArgs, cStdoutI, cStderrI)
	}

	wg.Wait()

	// Determine aggregate client status: first non-zero wins.
	clientStatus := 0
	for _, r := range results {
		if r.status != 0 && clientStatus == 0 {
			clientStatus = r.status
		}
	}

	// Combine per-proc CSVs and bins if all clients succeeded.
	if clientStatus == 0 {
		if err := result.CombineClients(clientCSVs, binsR, binsU, cOut, opts.Conns); err != nil {
			clientStatus = 1
		}
	}

	// Concatenate per-proc stdout/stderr into the main log files.
	concatFiles(procStdouts, cStdout)
	concatFiles(procStderrs, cStderr)

	return clientStatus
}

// replaceArg replaces the first argument starting with prefix in args.
// If no match is found the original slice is returned unchanged.
func replaceArg(args []string, prefix, replacement string) []string {
	for i, a := range args {
		if strings.HasPrefix(a, prefix) {
			args[i] = replacement
			return args
		}
	}
	return args
}

// concatFiles concatenates the contents of srcPaths into dst.
func concatFiles(srcPaths []string, dst string) {
	out, err := os.Create(dst)
	if err != nil {
		return
	}
	defer out.Close()
	for _, p := range srcPaths {
		f, err := os.Open(p)
		if err != nil {
			continue
		}
		io.Copy(out, f)
		f.Close()
	}
}

// writeEmpty creates an empty file (or truncates an existing one).
func writeEmpty(path string) {
	os.WriteFile(path, nil, 0o644)
}
