package run

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/control"
	"github.com/neguse/rudp-bench/orchestrator/netops"
	"github.com/neguse/rudp-bench/orchestrator/proc"
	"github.com/neguse/rudp-bench/orchestrator/sampler"
)

const metricsOutEnv = "BENCH_METRICS_OUT"

type processHandle struct {
	resultIndex int
	cmd         *exec.Cmd
	stdout      io.Closer
	stderr      io.Closer
}

type waitEvent struct {
	resultIndex int
	err         error
	exitCode    int
	errText     string
}

type controlEvent struct {
	result *control.Result
	err    error
}

type sampleEvent struct {
	series map[int]sampler.Series
	err    error
}

func Run(ctx context.Context, cfg RunConfig) (*Result, error) {
	cfg = cfg.withDefaults()
	if err := cfg.validate(); err != nil {
		return nil, err
	}
	if err := os.MkdirAll(cfg.OutputDir, 0o755); err != nil {
		return nil, fmt.Errorf("mkdir output_dir: %w", err)
	}
	logDir := filepath.Join(cfg.OutputDir, "logs")
	metricsDir := filepath.Join(cfg.OutputDir, "metrics")
	for _, path := range []string{filepath.Join(cfg.OutputDir, "result.json"), filepath.Join(cfg.OutputDir, "summary.txt"), logDir, metricsDir} {
		if _, err := os.Stat(path); err == nil {
			return nil, fmt.Errorf("output_dir already contains run artifact %s; use a fresh output_dir", path)
		} else if !errors.Is(err, os.ErrNotExist) {
			return nil, fmt.Errorf("inspect output artifact %s: %w", path, err)
		}
	}
	if err := os.Mkdir(logDir, 0o755); err != nil {
		return nil, fmt.Errorf("mkdir logs dir: %w", err)
	}
	if err := os.Mkdir(metricsDir, 0o755); err != nil {
		return nil, fmt.Errorf("mkdir metrics dir: %w", err)
	}
	// sudo 実行時、権限降下したベンチマークプロセスが metrics を書けるよう
	// 出力ディレクトリを元ユーザーに chown する
	if err := chownDirsToSudoUser(cfg.OutputDir, logDir, metricsDir); err != nil {
		return nil, err
	}

	resultPath := filepath.Join(cfg.OutputDir, "result.json")
	summaryPath := filepath.Join(cfg.OutputDir, "summary.txt")
	result := &Result{
		Version:   2,
		Transport: cfg.Transport,
		Outcome:   OutcomeInvalid,
		Verdict:   VerdictInvalid,
		Config:    cfg,
		Artifacts: map[string]string{
			"result_json": resultPath,
			"summary":     summaryPath,
		},
	}

	runCtx, cancelRun := context.WithCancel(ctx)
	defer cancelRun()

	var extraReasons, sutFailureReasons []string
	treatment := collectTreatment(ctx, cfg)
	result.Treatment = &treatment
	treatmentValidation := classifyScenarioTreatment(treatment, cfg)
	for _, reason := range treatmentValidation.Invalid {
		extraReasons = append(extraReasons, "treatment contract: "+reason)
	}
	if len(treatmentValidation.Unsupported) > 0 {
		result.Outcome = OutcomeUnsupported
		result.OutcomeReasons = append([]string(nil), treatmentValidation.Unsupported...)
		result.Verdict = VerdictInvalid
		result.InvalidReasons = nil
		return persistRunResult(result, resultPath, summaryPath)
	}
	if len(extraReasons) > 0 {
		return finishRunResult(result, GateInput{ExtraReasons: extraReasons}, resultPath, summaryPath)
	}
	var netemEnabled bool
	var udpDelta, serverUDPDelta netops.UDPStats
	var teardown []netops.Command
	if cfg.Netem != nil {
		netemEnabled = true
		pair := cfg.Netem.pairSpec()
		setup, err := netops.BuildSetupCommands(pair)
		if err != nil {
			extraReasons = append(extraReasons, fmt.Sprintf("build netns setup commands: %v", err))
			return finishRunResult(result, GateInput{ExtraReasons: extraReasons}, resultPath, summaryPath)
		}
		teardown, err = netops.BuildTeardownCommands(pair)
		if err != nil {
			extraReasons = append(extraReasons, fmt.Sprintf("build netns teardown commands: %v", err))
			return finishRunResult(result, GateInput{ExtraReasons: extraReasons}, resultPath, summaryPath)
		}
		result.Netem = &NetemResult{
			Enabled:          true,
			Pair:             pair,
			SetupCommands:    setup,
			TeardownCommands: teardown,
		}
		if err := netops.RunCommands(runCtx, setup, netops.RunOptions{}); err != nil {
			extraReasons = append(extraReasons, fmt.Sprintf("netns setup failed: %v", err))
			return finishRunResult(result, GateInput{ExtraReasons: extraReasons, NetemEnabled: true}, resultPath, summaryPath)
		}
		defer teardownNetem(teardown)

		// netem 実効値 gate(校正 §3): ping/iperf3 を実際に流し、実測の
		// RTT/loss が設定と一致することをベンチ開始前に機械検証する。
		// tc の echo back だけでは「設定はあるが実効が違う」型を検出できない。
		if !cfg.NetemGateOff {
			gateReport, err := netops.RunNetemGate(runCtx, pair)
			result.Netem.Gate = &gateReport
			if err != nil {
				extraReasons = append(extraReasons, fmt.Sprintf("netem gate tooling failed: %v", err))
				return finishRunResult(result, GateInput{ExtraReasons: extraReasons, NetemEnabled: true}, resultPath, summaryPath)
			}
			if !gateReport.OK() {
				for _, f := range gateReport.Failures {
					extraReasons = append(extraReasons, "netem gate: "+f)
				}
				return finishRunResult(result, GateInput{ExtraReasons: extraReasons, NetemEnabled: true}, resultPath, summaryPath)
			}
		}

		// 決定的 loss 注入時、gate(ping/iperf3)が消費した trace 位置を
		// 0 に戻す。計測本体が毎 run 同じ位置から trace を再生するための要件
		if netops.LossTraceEnabled(pair) {
			if err := netops.LossTraceReset(pair); err != nil {
				extraReasons = append(extraReasons, fmt.Sprintf("loss trace reset failed: %v", err))
				return finishRunResult(result, GateInput{ExtraReasons: extraReasons, NetemEnabled: true}, resultPath, summaryPath)
			}
		}

		before, err := readNetnsUDPStats(runCtx, pair.ClientNS)
		if err != nil {
			extraReasons = append(extraReasons, fmt.Sprintf("read client netns UDP counters before run: %v", err))
		}
		result.Netem.UDPBefore = before
		serverBefore, err := readNetnsUDPStats(runCtx, pair.ServerNS)
		if err != nil {
			extraReasons = append(extraReasons, fmt.Sprintf("read server netns UDP counters before run: %v", err))
		}
		result.Netem.ServerUDPBefore = serverBefore
	}

	sock := filepath.Join(os.TempDir(), fmt.Sprintf("rudp-bench-%d-%d.sock", os.Getpid(), time.Now().UnixNano()))
	expSent, expRecv := steadyExpectedRates(cfg)
	ctrl, err := control.NewServer(control.Config{
		SocketPath:                sock,
		Expected:                  cfg.ClientProcs + 1,
		Warmup:                    cfg.Warmup.Duration,
		SteadyWarmup:              cfg.SteadyWarmup,
		SteadyMinWarmup:           cfg.SteadyMinWarmup.Duration,
		SteadyExpectedSentPerConn: expSent,
		SteadyExpectedRecvPerConn: expRecv,
		Duration:                  cfg.Duration.Duration,
		Drain:                     cfg.Drain.Duration,
		HelloTimeout:              cfg.ControlTimeout.Duration,
		ReadyTimeout:              cfg.ControlTimeout.Duration,
		AckTimeout:                cfg.ControlTimeout.Duration,
		// done は schedule ack 直後から計時され、計測窓全体を跨いで待つ
		// (steady 時は warmup が上限なのでこの見積りが最悪ケースのまま成立)
		DoneTimeout: cfg.Warmup.Duration + cfg.Duration.Duration + cfg.Drain.Duration + cfg.ControlTimeout.Duration,
	})
	if err != nil {
		extraReasons = append(extraReasons, fmt.Sprintf("control server setup failed: %v", err))
		_, writeErr := finishRunResult(result, GateInput{ExtraReasons: extraReasons, NetemEnabled: netemEnabled}, resultPath, summaryPath)
		return result, errors.Join(err, writeErr)
	}

	controlCh := make(chan controlEvent, 1)
	go func() {
		r, err := ctrl.Run(runCtx)
		controlCh <- controlEvent{result: r, err: err}
	}()

	var handles []processHandle
	waitCh := make(chan waitEvent, cfg.ClientProcs+1)
	startedOK := true
	serverNS, clientNS := "", ""
	if cfg.Netem != nil {
		pair := cfg.Netem.pairSpec()
		serverNS = pair.ServerNS
		clientNS = pair.ClientNS
	}

	serverMetrics := filepath.Join(metricsDir, "server.json")
	serverCmd := expandCommandTemplate(cfg.ServerCommand, templateVars{
		Transport:  cfg.Transport,
		ProcIndex:  -1,
		TotalConns: cfg.TotalConns,
	})
	if err := startOne(runCtx, ctrl.SocketPath(), logDir, serverCmd, serverNS, cfg.ServerCPUs, ProcessResult{
		Role:       "server",
		ProcIndex:  -1,
		ExitCode:   -1,
		MetricsOut: serverMetrics,
	}, &result.Processes, &handles, waitCh); err != nil {
		startedOK = false
		extraReasons = append(extraReasons, fmt.Sprintf("start server: %v", err))
	}

	// server の listen 完了(control 上の ready)を待ってから client を起動する。
	// 起動の遅い server(cert 生成・ランタイム初期化)に client が先に接続を
	// 試みると、接続失敗 → 早期 exit → barrier abort の連鎖で run 全体が落ちる。
	waitsConsumed := 0
	if startedOK {
		select {
		case <-ctrl.ServerReady():
		case <-runCtx.Done():
			startedOK = false
			extraReasons = append(extraReasons, "canceled while waiting for server ready")
		case ev := <-waitCh:
			applyWaitEvent(ev, result.Processes)
			waitsConsumed++
			startedOK = false
			reason := "server exited before becoming ready: " + describeWait(result.Processes[ev.resultIndex], ev)
			extraReasons = append(extraReasons, reason)
			sutFailureReasons = append(sutFailureReasons, reason)
		case <-time.After(cfg.ControlTimeout.Duration):
			startedOK = false
			extraReasons = append(extraReasons, "server did not become ready before control timeout")
		}
	}

	connSplit := splitConns(cfg.TotalConns, cfg.ClientProcs)
	originStart := 0
	clientMetricPaths := make([]string, 0, cfg.ClientProcs)
	if startedOK {
		for i, conns := range connSplit {
			originEnd := originStart + conns
			metricsOut := filepath.Join(metricsDir, fmt.Sprintf("client-%d.json", i))
			clientMetricPaths = append(clientMetricPaths, metricsOut)
			clientCmd := expandCommandTemplate(cfg.ClientCommand, templateVars{
				Transport:     cfg.Transport,
				ProcIndex:     i,
				Conns:         conns,
				OriginIDStart: originStart,
				OriginIDEnd:   originEnd,
				TotalConns:    cfg.TotalConns,
			})
			err := startOne(runCtx, ctrl.SocketPath(), logDir, clientCmd, clientNS, cfg.ClientCPUs, ProcessResult{
				Role:          "client",
				ProcIndex:     i,
				Conns:         conns,
				OriginIDStart: originStart,
				OriginIDEnd:   originEnd,
				ExitCode:      -1,
				MetricsOut:    metricsOut,
			}, &result.Processes, &handles, waitCh)
			if err != nil {
				startedOK = false
				extraReasons = append(extraReasons, fmt.Sprintf("start client proc_index=%d: %v", i, err))
				break
			}
			originStart = originEnd
		}
	}

	if !startedOK {
		cancelRun()
		drainWaits(waitCh, len(handles)-waitsConsumed, cfg.ProcessExitTimeout.Duration, result.Processes)
		controlResult := awaitControl(controlCh, cfg.ControlTimeout.Duration)
		result.Control = controlResult.result
		if controlResult.err != nil && !errors.Is(controlResult.err, context.Canceled) {
			extraReasons = append(extraReasons, fmt.Sprintf("control barrier failed: %v", controlResult.err))
		}
		return finishRunResult(result, GateInput{
			Control:            result.Control,
			Processes:          result.Processes,
			AttemptedThreshold: cfg.AttemptedThreshold,
			NetemEnabled:       netemEnabled,
			UDPDropDelta:       udpDelta,
			ExtraReasons:       extraReasons,
			SUTFailureReasons:  sutFailureReasons,
		}, resultPath, summaryPath)
	}

	for _, h := range handles {
		if h.cmd.Process != nil {
			result.Processes[h.resultIndex].PID = h.cmd.Process.Pid
		}
	}

	pids := make([]int, 0, len(handles))
	for _, h := range handles {
		if h.cmd.Process != nil {
			pids = append(pids, h.cmd.Process.Pid)
		}
	}
	sampleCtx, stopSampler := context.WithCancel(runCtx)
	sampleCh := make(chan sampleEvent, 1)
	go func() {
		series, err := collectSamples(sampleCtx, pids, cfg.SamplerInterval.Duration)
		sampleCh <- sampleEvent{series: series, err: err}
	}()

	remaining := len(handles) - waitsConsumed
	controlDone := false
	for !controlDone {
		select {
		case ev := <-waitCh:
			applyWaitEvent(ev, result.Processes)
			remaining--
			if ev.err != nil {
				reason := fmt.Sprintf("process exited before control barrier completed: %s", describeWait(result.Processes[ev.resultIndex], ev))
				extraReasons = append(extraReasons, reason)
				sutFailureReasons = append(sutFailureReasons, reason)
				cancelRun()
				controlDone = true
			}
		case ev := <-controlCh:
			result.Control = ev.result
			if ev.err != nil {
				extraReasons = append(extraReasons, fmt.Sprintf("control barrier failed: %v", ev.err))
				cancelRun()
			}
			controlDone = true
		case <-ctx.Done():
			extraReasons = append(extraReasons, fmt.Sprintf("run context canceled: %v", ctx.Err()))
			cancelRun()
			controlDone = true
		}
	}

	if result.Control == nil {
		ev := awaitControl(controlCh, cfg.ControlTimeout.Duration)
		result.Control = ev.result
		if ev.err != nil && !errors.Is(ev.err, context.Canceled) {
			extraReasons = append(extraReasons, fmt.Sprintf("control barrier failed: %v", ev.err))
		}
	}

	if remaining > 0 {
		if timedOut := drainWaits(waitCh, remaining, cfg.ProcessExitTimeout.Duration, result.Processes); timedOut {
			reason := fmt.Sprintf("timed out waiting %s for processes to exit", cfg.ProcessExitTimeout.Duration)
			extraReasons = append(extraReasons, reason)
			if ctx.Err() == nil {
				sutFailureReasons = append(sutFailureReasons, reason)
			}
			cancelRun()
			drainWaits(waitCh, remainingExitedCount(result.Processes), cfg.ProcessExitTimeout.Duration, result.Processes)
		}
	}
	if ctx.Err() == nil {
		for _, process := range result.Processes {
			if process.Exited && process.ExitCode != 0 {
				sutFailureReasons = append(sutFailureReasons, fmt.Sprintf("%s proc_index=%d pid=%d exit_code=%d", process.Role, process.ProcIndex, process.PID, process.ExitCode))
			}
		}
	}

	stopSampler()
	samples := <-sampleCh
	if samples.err != nil {
		extraReasons = append(extraReasons, fmt.Sprintf("sampler failed: %v", samples.err))
	}
	if result.Control != nil {
		windowed := sampler.WindowAll(samples.series, result.Control.Schedule.StartAtNS, result.Control.Schedule.StopAtNS)
		result.Samples = sortedSeries(windowed)
	} else {
		result.Samples = sortedSeries(samples.series)
	}

	if cfg.Netem != nil && result.Netem != nil {
		pair := cfg.Netem.pairSpec()
		after, err := readNetnsUDPStats(context.Background(), pair.ClientNS)
		if err != nil {
			extraReasons = append(extraReasons, fmt.Sprintf("read client netns UDP counters after run: %v", err))
		}
		result.Netem.UDPAfter = after
		udpDelta = netops.DeltaUDPStats(result.Netem.UDPBefore, after)
		result.Netem.UDPDelta = udpDelta
		serverAfter, err := readNetnsUDPStats(context.Background(), pair.ServerNS)
		if err != nil {
			extraReasons = append(extraReasons, fmt.Sprintf("read server netns UDP counters after run: %v", err))
		}
		result.Netem.ServerUDPAfter = serverAfter
		serverUDPDelta = netops.DeltaUDPStats(result.Netem.ServerUDPBefore, serverAfter)
		result.Netem.ServerUDPDelta = serverUDPDelta
	}

	metricPaths := clientMetricPaths
	if cfg.Scenario != nil && cfg.Scenario.Kind == ScenarioAuthoritativeState {
		metricPaths = append([]string{serverMetrics}, clientMetricPaths...)
	}
	metrics, err := MergeMetricsFiles(metricPaths, cfg.TotalConns)
	if err == nil && cfg.Scenario != nil {
		err = ValidateScenarioMetricsFiles(serverMetrics, clientMetricPaths, connSplit, cfg.TotalConns,
			cfg.Duration.Duration, cfg.StalenessPeriodNS, *cfg.Scenario, metrics)
	}
	if err != nil {
		extraReasons = append(extraReasons, fmt.Sprintf("metrics contract: %v", err))
	} else {
		result.Metrics = metrics
	}

	return finishRunResult(result, GateInput{
		Control:            result.Control,
		Metrics:            result.Metrics,
		Processes:          result.Processes,
		AttemptedThreshold: cfg.AttemptedThreshold,
		NetemEnabled:       netemEnabled,
		UDPDropDelta:       udpDelta,
		ServerUDPDropDelta: serverUDPDelta,
		ExtraReasons:       extraReasons,
		SUTFailureReasons:  sutFailureReasons,
	}, resultPath, summaryPath)
}

func startOne(ctx context.Context, controlSock, logDir string, cmdCfg CommandConfig, netns, cpus string, pr ProcessResult, processes *[]ProcessResult, handles *[]processHandle, waitCh chan<- waitEvent) error {
	path, args := namespaceCommand(cmdCfg, netns, cpus)
	pr.Command = append([]string{path}, args...)
	stdoutPath := filepath.Join(logDir, fmt.Sprintf("%s-%d.stdout.log", pr.Role, pr.ProcIndex))
	stderrPath := filepath.Join(logDir, fmt.Sprintf("%s-%d.stderr.log", pr.Role, pr.ProcIndex))
	stdout, err := os.Create(stdoutPath)
	if err != nil {
		return fmt.Errorf("create stdout log: %w", err)
	}
	stderr, err := os.Create(stderrPath)
	if err != nil {
		_ = stdout.Close()
		return fmt.Errorf("create stderr log: %w", err)
	}
	pr.StdoutPath = stdoutPath
	pr.StderrPath = stderrPath
	env := append([]string(nil), cmdCfg.Env...)
	if pr.MetricsOut != "" {
		env = append(env, metricsOutEnv+"="+pr.MetricsOut)
	}
	cmd, err := proc.Start(ctx, proc.Spec{
		Path:        path,
		Args:        args,
		Env:         env,
		Dir:         cmdCfg.Dir,
		ControlSock: controlSock,
		Stdout:      stdout,
		Stderr:      stderr,
	})
	if err != nil {
		_ = stdout.Close()
		_ = stderr.Close()
		return err
	}
	pr.PID = cmd.Process.Pid
	index := len(*processes)
	*processes = append(*processes, pr)
	*handles = append(*handles, processHandle{resultIndex: index, cmd: cmd, stdout: stdout, stderr: stderr})
	go waitProcess(index, cmd, stdout, stderr, waitCh)
	return nil
}

// sudo 実行時に出力ディレクトリを SUDO_UID/SUDO_GID へ chown する。
// 非 root や SUDO_* 不在なら何もしない。
func chownDirsToSudoUser(dirs ...string) error {
	if os.Geteuid() != 0 {
		return nil
	}
	uidStr, gidStr := os.Getenv("SUDO_UID"), os.Getenv("SUDO_GID")
	if uidStr == "" || gidStr == "" {
		return nil
	}
	uid, err := strconv.Atoi(uidStr)
	if err != nil {
		return fmt.Errorf("parse SUDO_UID: %w", err)
	}
	gid, err := strconv.Atoi(gidStr)
	if err != nil {
		return fmt.Errorf("parse SUDO_GID: %w", err)
	}
	for _, d := range dirs {
		if err := os.Chown(d, uid, gid); err != nil {
			return fmt.Errorf("chown %s: %w", d, err)
		}
	}
	return nil
}

func namespaceCommand(cmd CommandConfig, netns, cpus string) (string, []string) {
	if netns == "" {
		if cpus != "" {
			return "taskset", append([]string{"-c", cpus, cmd.Path}, cmd.Args...)
		}
		return cmd.Path, append([]string(nil), cmd.Args...)
	}
	args := make([]string, 0, 12+len(cmd.Args))
	args = append(args, "netns", "exec", netns)
	// sudo 実行時はベンチマークプロセスを元ユーザーに降格する。
	// root のままだと msquic が datapath 初期化で SIGABRT する(v1 の教訓)ほか、
	// 結果ファイルの所有権も root になってしまう。netns 進入には root が必要な
	// ため、ip netns exec の内側で setpriv により降格する。
	if os.Geteuid() == 0 {
		if uid, gid := os.Getenv("SUDO_UID"), os.Getenv("SUDO_GID"); uid != "" && gid != "" {
			args = append(args, "setpriv", "--reuid", uid, "--regid", gid, "--init-groups")
		}
	}
	// 役割別 CPU 割当(v1 の役割隔離)。setpriv 後に置くことで降格ユーザーの
	// プロセスに affinity が掛かる
	if cpus != "" {
		args = append(args, "taskset", "-c", cpus)
	}
	args = append(args, cmd.Path)
	args = append(args, cmd.Args...)
	return "ip", args
}

func waitProcess(index int, cmd *exec.Cmd, stdout, stderr io.Closer, waitCh chan<- waitEvent) {
	err := cmd.Wait()
	_ = stdout.Close()
	_ = stderr.Close()
	code, text := waitErrorInfo(err)
	waitCh <- waitEvent{resultIndex: index, err: err, exitCode: code, errText: text}
}

func waitErrorInfo(err error) (int, string) {
	if err == nil {
		return 0, ""
	}
	var exitErr *exec.ExitError
	if errors.As(err, &exitErr) {
		if status, ok := exitErr.Sys().(syscall.WaitStatus); ok {
			if status.Signaled() {
				return 128 + int(status.Signal()), err.Error()
			}
			return status.ExitStatus(), err.Error()
		}
		return exitErr.ExitCode(), err.Error()
	}
	return -1, err.Error()
}

func applyWaitEvent(ev waitEvent, processes []ProcessResult) {
	if ev.resultIndex < 0 || ev.resultIndex >= len(processes) {
		return
	}
	processes[ev.resultIndex].Exited = true
	processes[ev.resultIndex].ExitCode = ev.exitCode
	processes[ev.resultIndex].Error = ev.errText
}

func drainWaits(waitCh <-chan waitEvent, remaining int, timeout time.Duration, processes []ProcessResult) bool {
	timer := time.NewTimer(timeout)
	defer timer.Stop()
	for remaining > 0 {
		select {
		case ev := <-waitCh:
			applyWaitEvent(ev, processes)
			remaining--
		case <-timer.C:
			return true
		}
	}
	return false
}

func remainingExitedCount(processes []ProcessResult) int {
	n := 0
	for _, p := range processes {
		if !p.Exited {
			n++
		}
	}
	return n
}

// steadyExpectedRates は定常判定の下限チェックに使う per-conn 期待レート
// (送信 / 受信、loss 調整済み)を返す。workload 名が未指定(明示 CLI 引数の
// 開発用 config 等)なら 0 を返し、下限チェックは無効(flat 判定のみ)になる。
func steadyExpectedRates(cfg RunConfig) (sent, recv float64) {
	if !cfg.SteadyWarmup {
		return 0, 0
	}
	if cfg.Scenario != nil {
		lossClient, lossServer := 1.0, 1.0
		if cfg.Netem != nil {
			lossClient = 1 - cfg.Netem.ClientEgress.LossPercent/100
			lossServer = 1 - cfg.Netem.ServerEgress.LossPercent/100
		}
		switch cfg.Scenario.Kind {
		case ScenarioEnvironmentBaseline:
			if cfg.Scenario.ClientInput != nil {
				sent = cfg.Scenario.ClientInput.rate()
				recv = cfg.Scenario.ClientInput.LossTolerant.RateHz*lossClient*lossServer +
					cfg.Scenario.ClientInput.MustDeliver.RateHz
			}
		case ScenarioAuthoritativeState:
			if cfg.Scenario.ClientInput != nil {
				sent = cfg.Scenario.ClientInput.rate()
			}
			if cfg.Scenario.ServerState != nil {
				recv = cfg.Scenario.ServerState.LossTolerant.RateHz*lossServer +
					cfg.Scenario.ServerState.MustDeliver.RateHz
			}
		case ScenarioRoomRelay:
			if cfg.Scenario.RoomPublish != nil {
				sent = cfg.Scenario.RoomPublish.rate()
				recv = cfg.Scenario.RoomPublish.LossTolerant.RateHz*float64(cfg.TotalConns)*lossClient*lossServer +
					cfg.Scenario.RoomPublish.MustDeliver.RateHz*float64(cfg.TotalConns)
			}
		}
		return sent, recv
	}
	if cfg.Workload == "" {
		return 0, 0
	}
	w, ok := LookupWorkload(cfg.Workload)
	if !ok {
		return 0, 0
	}
	// 経路係数: forward(origin の client_egress)+ return(server_egress)。
	// echo / broadcast どちらも受信までに両 egress を1回ずつ通る
	pf := 1.0
	if cfg.Netem != nil {
		pf = (1 - cfg.Netem.ClientEgress.LossPercent/100) *
			(1 - cfg.Netem.ServerEgress.LossPercent/100)
	}
	sent = w.RateLT + w.RateMD
	if w.RateLT > 0 {
		r := w.RateLT
		if w.BroadcastLT {
			r *= float64(cfg.TotalConns)
		}
		// loss-tolerant は落ちた分がそのまま減る
		recv += r * pf
	}
	if w.RateMD > 0 {
		r := w.RateMD
		if w.BroadcastMD {
			r *= float64(cfg.TotalConns)
		}
		// must-deliver は再送で届くので loss 係数を掛けない
		recv += r
	}
	return sent, recv
}

func awaitControl(controlCh <-chan controlEvent, timeout time.Duration) controlEvent {
	timer := time.NewTimer(timeout)
	defer timer.Stop()
	select {
	case ev := <-controlCh:
		return ev
	case <-timer.C:
		return controlEvent{err: fmt.Errorf("timed out waiting for control goroutine")}
	}
}

func describeWait(pr ProcessResult, ev waitEvent) string {
	if ev.errText != "" {
		return fmt.Sprintf("%s proc_index=%d pid=%d exit_code=%d: %s", pr.Role, pr.ProcIndex, pr.PID, ev.exitCode, ev.errText)
	}
	return fmt.Sprintf("%s proc_index=%d pid=%d exit_code=%d", pr.Role, pr.ProcIndex, pr.PID, ev.exitCode)
}

func collectSamples(ctx context.Context, pids []int, interval time.Duration) (map[int]sampler.Series, error) {
	if interval == 0 {
		interval = sampler.DefaultInterval
	}
	series := make(map[int]sampler.Series, len(pids))
	for _, pid := range pids {
		series[pid] = sampler.Series{PID: pid}
	}
	if err := sampleOnce(series, pids); err != nil {
		return series, err
	}
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return series, nil
		case <-ticker.C:
			if err := sampleOnce(series, pids); err != nil {
				return series, err
			}
		}
	}
}

func sampleOnce(series map[int]sampler.Series, pids []int) error {
	for _, pid := range pids {
		s, err := sampler.Read(pid)
		if err != nil {
			if errors.Is(err, os.ErrNotExist) {
				continue
			}
			return err
		}
		current := series[pid]
		current.Samples = append(current.Samples, s)
		series[pid] = current
	}
	return nil
}

func sortedSeries(series map[int]sampler.Series) []sampler.Series {
	pids := make([]int, 0, len(series))
	for pid := range series {
		pids = append(pids, pid)
	}
	sort.Ints(pids)
	out := make([]sampler.Series, 0, len(pids))
	for _, pid := range pids {
		out = append(out, series[pid])
	}
	return out
}

func readNetnsUDPStats(ctx context.Context, ns string) (netops.UDPStats, error) {
	cmd := exec.CommandContext(ctx, "ip", "netns", "exec", ns, "cat", "/proc/net/snmp")
	out, err := cmd.Output()
	if err != nil {
		return netops.UDPStats{}, err
	}
	return netops.ParseUDPStats(string(out))
}

func teardownNetem(cmds []netops.Command) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_ = netops.RunCommands(ctx, cmds, netops.RunOptions{})
}

func finishRunResult(result *Result, gateInput GateInput, resultPath, summaryPath string) (*Result, error) {
	gateInput.Config = &result.Config
	gate := EvaluateGate(gateInput)
	result.Verdict = gate.Verdict
	result.InvalidReasons = gate.Reasons
	if result.Config.Scenario != nil && result.Metrics != nil {
		evaluation := EvaluateScenarioMetrics(result.Metrics, *result.Config.Scenario)
		result.ScenarioEvaluation = &evaluation
	}
	gateInput.SUTFailureReasons = gate.SUTFailureReasons
	result.Outcome, result.OutcomeReasons = classifyRunOutcome(result, gateInput)
	return persistRunResult(result, resultPath, summaryPath)
}

func classifyRunOutcome(result *Result, gateInput GateInput) (Outcome, []string) {
	if result.Outcome == OutcomeUnsupported {
		return result.Outcome, dedupeStrings(result.OutcomeReasons)
	}
	if len(gateInput.SUTFailureReasons) > 0 {
		if independent := independentInvalidReasons(result.InvalidReasons); len(independent) > 0 {
			return OutcomeInvalid, independent
		}
		return OutcomeFail, dedupeStrings(gateInput.SUTFailureReasons)
	}
	if result.Verdict != VerdictValid && !onlyAttemptedShortfall(result.InvalidReasons) {
		return OutcomeInvalid, append([]string(nil), result.InvalidReasons...)
	}
	if result.Config.Scenario == nil {
		return OutcomePass, nil
	}
	if result.ScenarioEvaluation == nil {
		return OutcomeInvalid, []string{"scenario evaluation is missing"}
	}
	if !result.ScenarioEvaluation.CompletePrimarySLOs && result.ScenarioEvaluation.OK {
		return OutcomeInconclusive, []string{"primary SLOs missing for " + strings.Join(result.ScenarioEvaluation.MissingPrimarySLOs, ", ")}
	}
	if result.ScenarioEvaluation.OK {
		return OutcomePass, nil
	}
	return OutcomeFail, []string{result.ScenarioEvaluation.Cause}
}

func independentInvalidReasons(reasons []string) []string {
	var independent []string
	for _, reason := range reasons {
		consequence := false
		for _, fragment := range []string{
			"control result is missing", "control barrier failed", "merged metrics are missing",
			"metrics contract:", "process exited before control barrier", "server exited before becoming ready",
			" exit_code=", " did not exit", "timed out waiting", "attempted_ratio=",
		} {
			if strings.Contains(reason, fragment) {
				consequence = true
				break
			}
		}
		if !consequence {
			independent = append(independent, reason)
		}
	}
	return dedupeStrings(independent)
}

func onlyAttemptedShortfall(reasons []string) bool {
	if len(reasons) == 0 {
		return false
	}
	for _, reason := range reasons {
		if !strings.Contains(reason, "attempted_ratio=") {
			return false
		}
	}
	return true
}

func persistRunResult(result *Result, resultPath, summaryPath string) (*Result, error) {
	if err := writeResultJSON(resultPath, result); err != nil {
		return result, err
	}
	if err := writeSummary(summaryPath, result); err != nil {
		return result, err
	}
	return result, nil
}

func writeResultJSON(path string, result *Result) error {
	f, err := os.Create(path)
	if err != nil {
		return fmt.Errorf("create result.json: %w", err)
	}
	defer f.Close()
	enc := json.NewEncoder(f)
	enc.SetIndent("", "  ")
	if err := enc.Encode(result); err != nil {
		return fmt.Errorf("write result.json: %w", err)
	}
	return nil
}
