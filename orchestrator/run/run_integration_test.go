package run

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/control"
	"github.com/neguse/rudp-bench/orchestrator/monotonic"
	"github.com/neguse/rudp-bench/orchestrator/netops"
)

func TestMain(m *testing.M) {
	if os.Getenv("RUDP_BENCH_FAKE_PROCESS") == "1" {
		for _, arg := range os.Args {
			if arg == "--describe" {
				fmt.Print(fakeDescription())
				os.Exit(0)
			}
		}
	}
	os.Exit(m.Run())
}

func fakeDescription() string {
	if os.Getenv("FAKE_DESCRIBE_MODE") == "invalid_unsupported" {
		return `{"transport":"fake","class_mapping":{"loss_tolerant":{"primitive":"","delivery":"best_effort","ordering":"unordered","realization":"native"},"must_deliver":{"primitive":"fake-reliable","delivery":"reliable","ordering":"ordered","realization":"native"}},"coalescing":"none","cc_algo":"none","thread_model":"single","encryption":false,"payload_pattern":"splitmix64-v1","wire_compression":"none","max_payload_bytes":65536,"scenarios":[],"tuning":[]}`
	}
	return `{"transport":"fake","class_mapping":{"loss_tolerant":{"primitive":"fake-best-effort","delivery":"best_effort","ordering":"unordered","realization":"native"},"must_deliver":{"primitive":"fake-reliable","delivery":"reliable","ordering":"ordered","realization":"native"}},"coalescing":"none","cc_algo":"none","thread_model":"single","encryption":false,"payload_pattern":"splitmix64-v1","wire_compression":"none","max_payload_bytes":65536,"scenarios":["environment_baseline","authoritative_state","room_relay"],"tuning":[]}`
}

func TestRunIntegrationLocalFakeProcesses(t *testing.T) {
	outDir := t.TempDir()
	helperArgs := []string{"-test.run=TestFakeProcessHelper", "--"}
	commonEnv := []string{
		"RUDP_BENCH_FAKE_PROCESS=1",
		"FAKE_TRANSPORT=fake",
	}
	cfg := RunConfig{
		Transport: "fake",
		ServerCommand: CommandConfig{
			Path: os.Args[0],
			Args: helperArgs,
			Env:  append(append([]string{}, commonEnv...), "FAKE_ROLE=server", "FAKE_PROC_INDEX=-1", "FAKE_CONNS=0"),
		},
		ClientCommand: CommandConfig{
			Path: os.Args[0],
			Args: helperArgs,
			Env: append(append([]string{}, commonEnv...),
				"FAKE_ROLE=client",
				"FAKE_PROC_INDEX={proc_index}",
				"FAKE_CONNS={conns}",
				"FAKE_ORIGIN_START={origin_id_start}",
				"FAKE_ORIGIN_END={origin_id_end}",
			),
		},
		ClientProcs:        2,
		TotalConns:         3,
		Warmup:             Duration{Duration: 20 * time.Millisecond},
		Duration:           Duration{Duration: 30 * time.Millisecond},
		Drain:              Duration{Duration: 10 * time.Millisecond},
		DeadlineNS:         1_000_000,
		StalenessPeriodNS:  10_000_000,
		OutputDir:          outDir,
		ControlTimeout:     Duration{Duration: 2 * time.Second},
		SamplerInterval:    Duration{Duration: 5 * time.Millisecond},
		ProcessExitTimeout: Duration{Duration: 2 * time.Second},
	}

	result, err := Run(context.Background(), cfg)
	if err != nil {
		if errors.Is(err, syscall.EPERM) {
			t.Skipf("AF_UNIX sockets are not permitted in this sandbox: %v", err)
		}
		t.Fatal(err)
	}
	if result.Verdict != VerdictValid {
		t.Fatalf("verdict = %s, reasons=%v", result.Verdict, result.InvalidReasons)
	}
	if result.Outcome != OutcomePass {
		t.Fatalf("outcome = %s, reasons=%v", result.Outcome, result.OutcomeReasons)
	}
	if len(result.Processes) != 3 {
		t.Fatalf("processes = %d, want 3", len(result.Processes))
	}
	if result.Metrics == nil {
		t.Fatal("metrics = nil")
	}
	if result.Treatment == nil || len(result.Treatment.Server.Description) == 0 || len(result.Treatment.Client.Description) == 0 {
		t.Fatalf("treatment = %+v", result.Treatment)
	}
	if !result.Treatment.ClassMapping.Match || result.Treatment.ClassMapping.ServerSHA256 == "" ||
		result.Treatment.ClassMapping.ServerSHA256 != result.Treatment.ClassMapping.ClientSHA256 {
		t.Fatalf("class mapping record = %+v", result.Treatment.ClassMapping)
	}
	loss := result.Metrics.Classes[ClassLossTolerant]
	if loss.Slots != 15 || loss.SlotsBroadcast != 3 || loss.ExpectedReceives != 21 || loss.DeliveredUnique != 21 {
		t.Fatalf("loss aggregate = %+v", loss)
	}
	if loss.DeliveryRatio != 1 || loss.AttemptedRatio != 1 {
		t.Fatalf("loss ratios delivery=%g attempted=%g", loss.DeliveryRatio, loss.AttemptedRatio)
	}
	for _, artifact := range []string{result.Artifacts["result_json"], result.Artifacts["summary"]} {
		if _, err := os.Stat(artifact); err != nil {
			t.Fatalf("artifact %s: %v", artifact, err)
		}
	}
}

func TestRunIntegrationRampSnapshots(t *testing.T) {
	templateCfg, templateServerBase, templateClientBase := rampSnapshotFixture(t, 1)
	writePassingAuthoritativeRampPhase(t, templateCfg, templateServerBase, templateClientBase, 0, 1, false)

	outDir := t.TempDir()
	helperArgs := []string{"-test.run=TestFakeProcessHelper", "--"}
	commonEnv := []string{
		"RUDP_BENCH_FAKE_PROCESS=1",
		"FAKE_TRANSPORT=fake",
		"FAKE_EXPECT_RAMP=1",
	}
	scenario := authoritativeFixture()
	cfg := RunConfig{
		Transport: "fake",
		Scenario:  &scenario,
		ServerCommand: CommandConfig{
			Path: os.Args[0], Args: helperArgs,
			Env: append(append([]string{}, commonEnv...),
				"FAKE_ROLE=server", "FAKE_PROC_INDEX=-1", "FAKE_CONNS=0",
				"FAKE_RAMP_METRICS_TEMPLATE="+rampSnapshotPath(templateServerBase, 0, 1)),
		},
		ClientCommand: CommandConfig{
			Path: os.Args[0], Args: helperArgs,
			Env: append(append([]string{}, commonEnv...),
				"FAKE_ROLE=client", "FAKE_PROC_INDEX=0", "FAKE_CONNS=1",
				"FAKE_ORIGIN_START=0", "FAKE_ORIGIN_END=1",
				"FAKE_RAMP_METRICS_TEMPLATE="+rampSnapshotPath(templateClientBase, 0, 1)),
		},
		ClientProcs:       1,
		TotalConns:        1,
		Warmup:            Duration{Duration: 100 * time.Millisecond},
		StalenessPeriodNS: defaultStalenessPeriodNS,
		Ramp: &RampConfig{
			StartConns: 1,
			StepConns:  2,
			Guard:      Duration{},
			Sample:     Duration{Duration: time.Second},
			Drain:      Duration{Duration: 100 * time.Millisecond},
		},
		OutputDir:          outDir,
		ControlTimeout:     Duration{Duration: 2 * time.Second},
		SamplerInterval:    Duration{Duration: 10 * time.Millisecond},
		ProcessExitTimeout: Duration{Duration: 2 * time.Second},
	}

	result, err := Run(context.Background(), cfg)
	if err != nil {
		if errors.Is(err, syscall.EPERM) {
			t.Skipf("AF_UNIX sockets are not permitted in this sandbox: %v", err)
		}
		t.Fatal(err)
	}
	if result.Verdict != VerdictValid || result.Outcome != OutcomeCensored {
		t.Fatalf("ramp verdict=%s outcome=%s invalid=%v outcome_reasons=%v",
			result.Verdict, result.Outcome, result.InvalidReasons, result.OutcomeReasons)
	}
	if result.Ramp == nil || !result.Ramp.Censored || len(result.Ramp.Timeline) != 1 {
		t.Fatalf("ramp result=%+v", result.Ramp)
	}
	if result.Metrics != nil {
		t.Fatalf("fixed final metrics unexpectedly populated: %+v", result.Metrics)
	}
}

func TestRunIntegrationRampStopsOnlineAtFirstBreak(t *testing.T) {
	templateCfg, templateServerBase, templateClientBase := rampSnapshotFixture(t, 1)
	writePassingAuthoritativeRampPhase(t, templateCfg, templateServerBase, templateClientBase, 0, 1, true)

	outDir := t.TempDir()
	helperArgs := []string{"-test.run=TestFakeProcessHelper", "--"}
	commonEnv := []string{
		"RUDP_BENCH_FAKE_PROCESS=1",
		"FAKE_TRANSPORT=fake",
		"FAKE_EXPECT_RAMP=1",
		"FAKE_RAMP_ONLINE=1",
	}
	scenario := authoritativeFixture()
	cfg := RunConfig{
		Transport: "fake",
		Scenario:  &scenario,
		ServerCommand: CommandConfig{
			Path: os.Args[0], Args: helperArgs,
			Env: append(append([]string{}, commonEnv...),
				"FAKE_ROLE=server", "FAKE_PROC_INDEX=-1", "FAKE_CONNS=0",
				"FAKE_RAMP_METRICS_TEMPLATE="+rampSnapshotPath(templateServerBase, 0, 1)),
		},
		ClientCommand: CommandConfig{
			Path: os.Args[0], Args: helperArgs,
			Env: append(append([]string{}, commonEnv...),
				"FAKE_ROLE=client", "FAKE_PROC_INDEX=0", "FAKE_CONNS=3",
				"FAKE_ORIGIN_START=0", "FAKE_ORIGIN_END=3",
				"FAKE_RAMP_METRICS_TEMPLATE="+rampSnapshotPath(templateClientBase, 0, 1)),
		},
		ClientProcs:       1,
		TotalConns:        3,
		Warmup:            Duration{Duration: 100 * time.Millisecond},
		StalenessPeriodNS: defaultStalenessPeriodNS,
		Ramp: &RampConfig{
			StartConns: 1,
			StepConns:  2,
			Sample:     Duration{Duration: time.Second},
			Drain:      Duration{Duration: 100 * time.Millisecond},
		},
		OutputDir:          outDir,
		ControlTimeout:     Duration{Duration: 2 * time.Second},
		SamplerInterval:    Duration{Duration: 10 * time.Millisecond},
		ProcessExitTimeout: Duration{Duration: 2 * time.Second},
	}

	result, err := Run(context.Background(), cfg)
	if err != nil {
		if errors.Is(err, syscall.EPERM) {
			t.Skipf("AF_UNIX sockets are not permitted in this sandbox: %v", err)
		}
		t.Fatal(err)
	}
	if result.Verdict != VerdictValid || result.Outcome != OutcomeFail ||
		result.Ramp == nil || result.Ramp.ScoreConns != 1 || len(result.Ramp.Timeline) != 1 {
		t.Fatalf("online ramp result=%+v", result)
	}
	if _, err := os.Stat(filepath.Join(outDir, "metrics", "ramp.stop")); err != nil {
		t.Fatalf("ramp stop marker: %v", err)
	}
	for _, base := range []string{
		filepath.Join(outDir, "metrics", "server.json"),
		filepath.Join(outDir, "metrics", "client-0.json"),
	} {
		if _, err := os.Stat(rampSnapshotPath(base, 1, 3)); !errors.Is(err, os.ErrNotExist) {
			t.Fatalf("unexpected tail snapshot %s: %v", base, err)
		}
	}
}

func TestRunTreatmentInvalidPrecedesUnsupported(t *testing.T) {
	descriptionEnv := []string{
		"RUDP_BENCH_FAKE_PROCESS=1",
		"FAKE_TRANSPORT=fake",
		"FAKE_DESCRIBE_MODE=invalid_unsupported",
	}
	cfg := RunConfig{
		Transport: "fake",
		Scenario:  ptr(authoritativeFixture()),
		ServerCommand: CommandConfig{
			Path: os.Args[0], Env: descriptionEnv,
		},
		ClientCommand: CommandConfig{
			Path: os.Args[0], Env: descriptionEnv,
		},
		ClientProcs: 1,
		TotalConns:  1,
		Duration:    Duration{Duration: time.Second},
		OutputDir:   t.TempDir(),
	}
	result, err := Run(context.Background(), cfg)
	if err != nil {
		t.Fatal(err)
	}
	if result.Outcome != OutcomeInvalid || result.Verdict != VerdictInvalid {
		t.Fatalf("outcome=%s verdict=%s reasons=%v", result.Outcome, result.Verdict, result.InvalidReasons)
	}
	if got := strings.Join(result.InvalidReasons, "; "); !strings.Contains(got, "primitive is empty") {
		t.Fatalf("invalid reasons = %v", result.InvalidReasons)
	}
}

func TestRunRejectsClassMappingDriftBeforeMeasurement(t *testing.T) {
	env := []string{"RUDP_BENCH_FAKE_PROCESS=1", "FAKE_TRANSPORT=fake"}
	cfg := RunConfig{
		Transport:          "fake",
		ClassMappingSHA256: strings.Repeat("f", 64),
		ServerCommand:      CommandConfig{Path: os.Args[0], Env: env},
		ClientCommand:      CommandConfig{Path: os.Args[0], Env: env},
		ClientProcs:        1,
		TotalConns:         1,
		Duration:           Duration{Duration: time.Second},
		OutputDir:          t.TempDir(),
	}
	result, err := Run(context.Background(), cfg)
	if err != nil {
		t.Fatal(err)
	}
	if result.Outcome != OutcomeInvalid ||
		!strings.Contains(strings.Join(result.InvalidReasons, "; "), "class_mapping_sha256 drift") {
		t.Fatalf("mapping drift result = %+v", result)
	}
}

func TestFakeProcessHelper(t *testing.T) {
	if os.Getenv("RUDP_BENCH_FAKE_PROCESS") != "1" {
		return
	}
	for _, arg := range os.Args {
		if arg == "--describe" {
			fmt.Print(fakeDescription())
			os.Exit(0)
		}
	}
	if err := fakeProcessMain(); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	os.Exit(0)
}

func fakeProcessMain() error {
	role := os.Getenv("FAKE_ROLE")
	transport := os.Getenv("FAKE_TRANSPORT")
	procIndex, err := strconv.Atoi(os.Getenv("FAKE_PROC_INDEX"))
	if err != nil {
		return fmt.Errorf("FAKE_PROC_INDEX: %w", err)
	}
	conns, err := strconv.Atoi(os.Getenv("FAKE_CONNS"))
	if err != nil {
		return fmt.Errorf("FAKE_CONNS: %w", err)
	}
	conn, err := dialControl(os.Getenv("BENCH_CONTROL_SOCK"))
	if err != nil {
		return err
	}
	defer conn.Close()
	if err := conn.SetDeadline(time.Now().Add(5 * time.Second)); err != nil {
		return err
	}

	enc := json.NewEncoder(conn)
	dec := json.NewDecoder(conn)
	if err := enc.Encode(control.HelloMessage{
		Type:      control.TypeHello,
		Role:      role,
		Transport: transport,
		PID:       os.Getpid(),
		ProcIndex: procIndex,
	}); err != nil {
		return err
	}
	if err := enc.Encode(control.ReadyMessage{Type: control.TypeReady, Conns: conns}); err != nil {
		return err
	}
	var schedule control.ScheduleMessage
	if err := dec.Decode(&schedule); err != nil {
		return err
	}
	now, err := monotonic.NowNS()
	if err != nil {
		return err
	}
	if err := enc.Encode(control.SchedAckMessage{Type: control.TypeSchedAck, MarginNS: schedule.StartAtNS - now}); err != nil {
		return err
	}
	if os.Getenv("FAKE_RAMP_ONLINE") == "1" {
		if err := writeFakeRampSnapshot(); err != nil {
			return err
		}
		stopPath := os.Getenv(rampStopPathEnv)
		deadline := time.Now().Add(2 * time.Second)
		for {
			if _, err := os.Stat(stopPath); err == nil {
				break
			} else if !errors.Is(err, os.ErrNotExist) {
				return err
			}
			if time.Now().After(deadline) {
				return fmt.Errorf("timed out waiting for ramp stop marker %s", stopPath)
			}
			time.Sleep(5 * time.Millisecond)
		}
		stats := json.RawMessage(fmt.Sprintf(`{"role":%q,"proc_index":%d}`, role, procIndex))
		return enc.Encode(control.DoneMessage{Type: control.TypeDone, Stats: stats})
	}
	if err := sleepUntilMonotonic(schedule.DrainUntilNS); err != nil {
		return err
	}
	if template := os.Getenv("FAKE_RAMP_METRICS_TEMPLATE"); template != "" {
		if err := writeFakeRampSnapshot(); err != nil {
			return err
		}
	} else if role == "client" {
		if err := os.WriteFile(os.Getenv(metricsOutEnv), []byte(fakeMetricsForProc(procIndex)), 0o644); err != nil {
			return err
		}
	}
	stats := json.RawMessage(fmt.Sprintf(`{"role":%q,"proc_index":%d}`, role, procIndex))
	return enc.Encode(control.DoneMessage{Type: control.TypeDone, Stats: stats})
}

func writeFakeRampSnapshot() error {
	for name, want := range map[string]string{
		rampStartConnsEnv: "1",
		rampStepConnsEnv:  "2",
		rampGuardNSEnv:    "0",
		rampSampleNSEnv:   "1000000000",
		rampDrainNSEnv:    "100000000",
	} {
		if os.Getenv(name) != want {
			return fmt.Errorf("%s=%q, want %q", name, os.Getenv(name), want)
		}
	}
	if os.Getenv(rampStopPathEnv) == "" {
		return fmt.Errorf("%s is empty", rampStopPathEnv)
	}
	template := os.Getenv("FAKE_RAMP_METRICS_TEMPLATE")
	data, err := os.ReadFile(template)
	if err != nil {
		return err
	}
	path := fmt.Sprintf("%s.ramp-%06d-c%06d.json", os.Getenv(metricsOutEnv), 0, 1)
	return os.WriteFile(path, data, 0o644)
}

func dialControl(path string) (net.Conn, error) {
	var lastErr error
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		conn, err := net.Dial("unix", path)
		if err == nil {
			return conn, nil
		}
		lastErr = err
		time.Sleep(5 * time.Millisecond)
	}
	return nil, lastErr
}

func sleepUntilMonotonic(targetNS int64) error {
	for {
		now, err := monotonic.NowNS()
		if err != nil {
			return err
		}
		if now >= targetNS {
			return nil
		}
		sleep := time.Duration(targetNS - now)
		if sleep > 10*time.Millisecond {
			sleep = 10 * time.Millisecond
		}
		time.Sleep(sleep)
	}
}

func fakeMetricsForProc(index int) string {
	zero := bins(nil)
	switch index {
	case 0:
		return metricsFixtureString(
			ClassCounts{Slots: 10, SlotsBroadcast: 2, Submitted: 10, DeliveredUnique: 14},
			ClassCounts{},
			bins(map[int]uint64{0: 14}),
			bins(map[int]uint64{1: 14}),
			zero,
			zero,
			bins(map[int]uint64{0: 3}),
			RawCounts{Slots: 10, Submitted: 10, RecvMeasured: 14},
		)
	default:
		return metricsFixtureString(
			ClassCounts{Slots: 5, SlotsBroadcast: 1, Submitted: 5, DeliveredUnique: 7},
			ClassCounts{},
			bins(map[int]uint64{0: 7}),
			bins(map[int]uint64{1: 7}),
			zero,
			zero,
			bins(map[int]uint64{0: 2}),
			RawCounts{Slots: 5, Submitted: 5, RecvMeasured: 7},
		)
	}
}

func TestPartialNetemTeardownOnlyDeletesOwnedNamespaces(t *testing.T) {
	commands := []netops.Command{
		{Name: "ip", Args: []string{"netns", "del", "server"}},
		{Name: "ip", Args: []string{"netns", "del", "client"}},
		{Name: "rm", Args: []string{"-rf", "/pins"}},
	}
	if got := partialNetemTeardown(commands, 0); got != nil {
		t.Fatalf("zero completed setup commands returned cleanup: %v", got)
	}
	if got := partialNetemTeardown(commands, 1); len(got) != 1 || got[0].Args[2] != "server" {
		t.Fatalf("server-only cleanup=%v", got)
	}
	if got := partialNetemTeardown(commands, 2); len(got) != len(commands) {
		t.Fatalf("full cleanup=%v", got)
	}
}

func TestTeardownNetemReturnsCommandFailures(t *testing.T) {
	missing := filepath.Join(t.TempDir(), "missing-teardown-command")
	err := teardownNetem([]netops.Command{{Name: missing}})
	if err == nil || !strings.Contains(err.Error(), missing) {
		t.Fatalf("teardown error=%v, want command failure", err)
	}
}
