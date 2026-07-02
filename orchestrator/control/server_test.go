package control

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"testing"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/monotonic"
)

func TestLifecycleRoundTrip(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	srv, dial := newTestServer(t, testConfig(t, 3))
	resultCh := make(chan runResult, 1)
	go func() {
		result, err := srv.Run(ctx)
		resultCh <- runResult{result: result, err: err}
	}()

	errCh := runFakeProcesses(ctx, dial, 3, func(schedule ScheduleMessage) (int64, error) {
		nowNS, err := monotonic.NowNS()
		if err != nil {
			return 0, err
		}
		return schedule.StartAtNS - nowNS, nil
	})

	result := awaitResult(t, resultCh)
	if result.err != nil {
		t.Fatalf("Run() error = %v", result.err)
	}
	if errs := collectErrors(errCh); len(errs) > 0 {
		t.Fatalf("fake processes failed: %v", errs)
	}
	if !result.result.Valid {
		t.Fatalf("result.Valid = false, reasons=%v", result.result.InvalidReasons)
	}
	if result.result.Schedule.Type != TypeSchedule {
		t.Fatalf("schedule type = %q", result.result.Schedule.Type)
	}
	if result.result.Schedule.StartAtNS <= 0 || result.result.Schedule.StopAtNS <= result.result.Schedule.StartAtNS || result.result.Schedule.DrainUntilNS <= result.result.Schedule.StopAtNS {
		t.Fatalf("schedule times not increasing: %+v", result.result.Schedule)
	}
	if got := len(result.result.Participants); got != 3 {
		t.Fatalf("participants = %d, want 3", got)
	}
	for i, p := range result.result.Participants {
		if p.Hello.ProcIndex != i {
			t.Fatalf("participant %d proc_index = %d", i, p.Hello.ProcIndex)
		}
		if !p.ReadyReceived || !p.AckReceived || !p.DoneReceived {
			t.Fatalf("participant %d did not complete: %+v", i, p)
		}
		var stats struct {
			Index int `json:"index"`
		}
		if err := json.Unmarshal(p.Done.Stats, &stats); err != nil {
			t.Fatalf("stats decode: %v", err)
		}
		if stats.Index != i {
			t.Fatalf("stats index = %d, want %d", stats.Index, i)
		}
	}
}

func TestReadyTimeout(t *testing.T) {
	ctx := context.Background()
	cfg := testConfig(t, 1)
	cfg.HelloTimeout = 200 * time.Millisecond
	cfg.ReadyTimeout = 30 * time.Millisecond
	srv, dial := newTestServer(t, cfg)
	errCh := make(chan error, 1)
	go func() {
		conn, err := dial(ctx)
		if err != nil {
			errCh <- err
			return
		}
		defer conn.Close()
		errCh <- json.NewEncoder(conn).Encode(HelloMessage{
			Type:      TypeHello,
			Role:      "client",
			Transport: "fake",
			PID:       123,
			ProcIndex: 0,
		})
	}()

	_, runErr := srv.Run(ctx)
	if err := <-errCh; err != nil {
		t.Fatalf("fake hello failed: %v", err)
	}
	var timeoutErr TimeoutError
	if !errors.As(runErr, &timeoutErr) {
		t.Fatalf("Run() error = %v, want TimeoutError", runErr)
	}
	if timeoutErr.Stage != StageReady {
		t.Fatalf("timeout stage = %s, want %s", timeoutErr.Stage, StageReady)
	}
}

func TestNegativeMarginInvalidatesRun(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	srv, dial := newTestServer(t, testConfig(t, 1))
	resultCh := make(chan runResult, 1)
	go func() {
		result, err := srv.Run(ctx)
		resultCh <- runResult{result: result, err: err}
	}()

	errCh := runFakeProcesses(ctx, dial, 1, func(ScheduleMessage) (int64, error) {
		return -1, nil
	})
	result := awaitResult(t, resultCh)
	if result.err != nil {
		t.Fatalf("Run() error = %v", result.err)
	}
	if errs := collectErrors(errCh); len(errs) > 0 {
		t.Fatalf("fake processes failed: %v", errs)
	}
	if result.result.Valid {
		t.Fatal("result.Valid = true, want false")
	}
	if len(result.result.InvalidReasons) != 1 || !strings.Contains(result.result.InvalidReasons[0], "negative schedule margin") {
		t.Fatalf("invalid reasons = %v", result.result.InvalidReasons)
	}
}

type runResult struct {
	result *Result
	err    error
}

type dialFunc func(context.Context) (net.Conn, error)

func newTestServer(t *testing.T, cfg Config) (*Server, dialFunc) {
	t.Helper()
	srv, err := NewServer(cfg)
	if err == nil {
		return srv, func(ctx context.Context) (net.Conn, error) {
			var d net.Dialer
			return d.DialContext(ctx, "unix", srv.SocketPath())
		}
	}
	if !errors.Is(err, syscall.EPERM) {
		t.Fatalf("NewServer() error = %v", err)
	}
	t.Logf("AF_UNIX sockets are not permitted in this sandbox; using net.Pipe listener for lifecycle coverage: %v", err)
	pl := newPipeListener()
	srv, err = newServerWithListener(cfg, pl)
	if err != nil {
		t.Fatal(err)
	}
	return srv, pl.DialContext
}

func testConfig(t *testing.T, expected int) Config {
	t.Helper()
	return Config{
		SocketPath:   filepath.Join(t.TempDir(), "sock"),
		Expected:     expected,
		Warmup:       80 * time.Millisecond,
		Duration:     20 * time.Millisecond,
		Drain:        10 * time.Millisecond,
		HelloTimeout: time.Second,
		ReadyTimeout: time.Second,
		AckTimeout:   time.Second,
		DoneTimeout:  time.Second,
	}
}

func runFakeProcesses(ctx context.Context, dial dialFunc, n int, margin func(ScheduleMessage) (int64, error)) <-chan error {
	errCh := make(chan error, n)
	var wg sync.WaitGroup
	wg.Add(n)
	for i := 0; i < n; i++ {
		i := i
		go func() {
			defer wg.Done()
			errCh <- fakeProcess(ctx, dial, i, margin)
		}()
	}
	go func() {
		wg.Wait()
		close(errCh)
	}()
	return errCh
}

func fakeProcess(ctx context.Context, dial dialFunc, index int, margin func(ScheduleMessage) (int64, error)) error {
	conn, err := dial(ctx)
	if err != nil {
		return err
	}
	defer conn.Close()
	if err := conn.SetDeadline(time.Now().Add(3 * time.Second)); err != nil {
		return err
	}
	enc := json.NewEncoder(conn)
	dec := json.NewDecoder(conn)
	if err := enc.Encode(HelloMessage{
		Type:      TypeHello,
		Role:      "client",
		Transport: "fake",
		PID:       1000 + index,
		ProcIndex: index,
	}); err != nil {
		return err
	}
	if err := enc.Encode(ReadyMessage{Type: TypeReady, Conns: 10 + index}); err != nil {
		return err
	}
	var schedule ScheduleMessage
	if err := dec.Decode(&schedule); err != nil {
		return err
	}
	if schedule.Type != TypeSchedule {
		return fmt.Errorf("schedule type = %q", schedule.Type)
	}
	marginNS, err := margin(schedule)
	if err != nil {
		return err
	}
	if err := enc.Encode(SchedAckMessage{Type: TypeSchedAck, MarginNS: marginNS}); err != nil {
		return err
	}
	stats := json.RawMessage(fmt.Sprintf(`{"index":%d}`, index))
	return enc.Encode(DoneMessage{Type: TypeDone, Stats: stats})
}

func awaitResult(t *testing.T, resultCh <-chan runResult) runResult {
	t.Helper()
	select {
	case result := <-resultCh:
		return result
	case <-time.After(5 * time.Second):
		t.Fatal("timed out waiting for control server result")
		return runResult{}
	}
}

func collectErrors(errCh <-chan error) []error {
	var errs []error
	for err := range errCh {
		if err != nil {
			errs = append(errs, err)
		}
	}
	return errs
}

type pipeListener struct {
	ch   chan net.Conn
	done chan struct{}
	once sync.Once
}

func newPipeListener() *pipeListener {
	return &pipeListener{
		ch:   make(chan net.Conn),
		done: make(chan struct{}),
	}
}

func (l *pipeListener) Accept() (net.Conn, error) {
	select {
	case conn := <-l.ch:
		return conn, nil
	case <-l.done:
		return nil, net.ErrClosed
	}
}

func (l *pipeListener) Close() error {
	l.once.Do(func() {
		close(l.done)
	})
	return nil
}

func (l *pipeListener) Addr() net.Addr {
	return pipeAddr("control-pipe")
}

func (l *pipeListener) DialContext(ctx context.Context) (net.Conn, error) {
	server, client := net.Pipe()
	select {
	case l.ch <- server:
		return client, nil
	case <-l.done:
		_ = server.Close()
		_ = client.Close()
		return nil, net.ErrClosed
	case <-ctx.Done():
		_ = server.Close()
		_ = client.Close()
		return nil, ctx.Err()
	}
}

type pipeAddr string

func (a pipeAddr) Network() string { return "pipe" }
func (a pipeAddr) String() string  { return string(a) }

var _ net.Listener = (*pipeListener)(nil)
var _ io.Closer = (*pipeListener)(nil)
