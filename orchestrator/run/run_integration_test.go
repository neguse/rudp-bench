package run

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"os"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/control"
	"github.com/neguse/rudp-bench/orchestrator/monotonic"
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
		return `{"transport":"fake","class_mapping":{"loss_tolerant":{"primitive":"","delivery":"best_effort","ordering":"unordered","realization":"native"},"must_deliver":{"primitive":"fake-reliable","delivery":"reliable","ordering":"ordered","realization":"native"}},"coalescing":"none","cc_algo":"none","thread_model":"single","encryption":false,"max_payload_bytes":65536,"scenarios":[],"tuning":[]}`
	}
	return `{"transport":"fake","class_mapping":{"loss_tolerant":{"primitive":"fake-best-effort","delivery":"best_effort","ordering":"unordered","realization":"native"},"must_deliver":{"primitive":"fake-reliable","delivery":"reliable","ordering":"ordered","realization":"native"}},"coalescing":"none","cc_algo":"none","thread_model":"single","encryption":false,"max_payload_bytes":65536,"scenarios":["environment_baseline","authoritative_state","room_relay"],"tuning":[]}`
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
	if err := sleepUntilMonotonic(schedule.DrainUntilNS); err != nil {
		return err
	}
	if role == "client" {
		if err := os.WriteFile(os.Getenv(metricsOutEnv), []byte(fakeMetricsForProc(procIndex)), 0o644); err != nil {
			return err
		}
	}
	stats := json.RawMessage(fmt.Sprintf(`{"role":%q,"proc_index":%d}`, role, procIndex))
	return enc.Encode(control.DoneMessage{Type: control.TypeDone, Stats: stats})
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
