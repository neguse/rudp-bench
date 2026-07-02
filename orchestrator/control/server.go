package control

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"path/filepath"
	"sort"
	"sync"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/monotonic"
)

type Stage string

const (
	StageHello Stage = "hello"
	StageReady Stage = "ready"
	StageAck   Stage = "sched_ack"
	StageDone  Stage = "done"
)

type TimeoutError struct {
	Stage   Stage
	Timeout time.Duration
}

func (e TimeoutError) Error() string {
	return fmt.Sprintf("control %s timeout after %s", e.Stage, e.Timeout)
}

type Config struct {
	SocketPath string
	Expected   int

	Warmup   time.Duration
	Duration time.Duration
	Drain    time.Duration

	HelloTimeout time.Duration
	ReadyTimeout time.Duration
	AckTimeout   time.Duration
	DoneTimeout  time.Duration
}

type Server struct {
	cfg          Config
	ln           net.Listener
	removeSocket bool

	mu     sync.Mutex
	nextID int
	conns  map[int]net.Conn
}

func NewServer(cfg Config) (*Server, error) {
	cfg = withDefaults(cfg)
	if err := validateConfig(cfg); err != nil {
		return nil, err
	}
	if err := os.MkdirAll(filepath.Dir(cfg.SocketPath), 0o755); err != nil {
		return nil, fmt.Errorf("mkdir control socket dir: %w", err)
	}
	if err := os.Remove(cfg.SocketPath); err != nil && !errors.Is(err, os.ErrNotExist) {
		return nil, fmt.Errorf("remove stale control socket: %w", err)
	}
	ln, err := listenUnix(cfg.SocketPath)
	if err != nil {
		return nil, fmt.Errorf("listen control socket: %w", err)
	}
	return &Server{
		cfg:          cfg,
		ln:           ln,
		removeSocket: true,
		conns:        make(map[int]net.Conn),
	}, nil
}

func newServerWithListener(cfg Config, ln net.Listener) (*Server, error) {
	cfg = withDefaults(cfg)
	if err := validateConfig(cfg); err != nil {
		return nil, err
	}
	return &Server{
		cfg:   cfg,
		ln:    ln,
		conns: make(map[int]net.Conn),
	}, nil
}

func listenUnix(path string) (*net.UnixListener, error) {
	fd, err := syscallSocketUnix()
	if err != nil {
		return nil, err
	}
	file := os.NewFile(uintptr(fd), "unix:"+path)
	closed := false
	closeFile := func() {
		if !closed {
			_ = file.Close()
			closed = true
		}
	}
	defer closeFile()

	if err := syscallBindUnix(fd, path); err != nil {
		return nil, err
	}
	if err := syscallListen(fd); err != nil {
		return nil, err
	}
	listener, err := net.FileListener(file)
	if err != nil {
		return nil, err
	}
	unixListener, ok := listener.(*net.UnixListener)
	if !ok {
		_ = listener.Close()
		return nil, fmt.Errorf("file listener is %T, want *net.UnixListener", listener)
	}
	return unixListener, nil
}

func Run(ctx context.Context, cfg Config) (*Result, error) {
	s, err := NewServer(cfg)
	if err != nil {
		return nil, err
	}
	return s.Run(ctx)
}

func (s *Server) SocketPath() string {
	return s.cfg.SocketPath
}

func (s *Server) Close() error {
	var errs []error
	if s.ln != nil {
		errs = append(errs, s.ln.Close())
	}
	s.mu.Lock()
	for _, c := range s.conns {
		errs = append(errs, c.Close())
	}
	s.mu.Unlock()
	if s.removeSocket {
		errs = append(errs, os.Remove(s.cfg.SocketPath))
	}
	return errors.Join(errs...)
}

func (s *Server) Run(ctx context.Context) (*Result, error) {
	ctx, cancel := context.WithCancel(ctx)

	events := make(chan event, max(16, s.cfg.Expected*4+4))
	var wg sync.WaitGroup
	wg.Add(1)
	go func() {
		defer wg.Done()
		s.acceptLoop(ctx, events)
	}()
	defer func() {
		cancel()
		_ = s.Close()
		wg.Wait()
	}()

	state := newRunState(s.cfg.Expected)
	stage := StageHello
	timer := time.NewTimer(s.cfg.HelloTimeout)
	defer timer.Stop()

	for {
		select {
		case <-ctx.Done():
			return state.result(), ctx.Err()
		case <-timer.C:
			return state.result(), TimeoutError{Stage: stage, Timeout: timeoutForStage(s.cfg, stage)}
		case ev := <-events:
			if ev.err != nil {
				return state.result(), ev.err
			}
			if err := state.apply(ev); err != nil {
				return state.result(), err
			}
			for {
				next, err := s.advanceStageIfReady(state, stage, timer)
				if err != nil {
					return state.result(), err
				}
				if next == stage {
					break
				}
				stage = next
				if stage == "" {
					return state.result(), nil
				}
			}
		}
	}
}

func (s *Server) advanceStageIfReady(state *runState, stage Stage, timer *time.Timer) (Stage, error) {
	switch stage {
	case StageHello:
		if state.helloCount() < s.cfg.Expected {
			return stage, nil
		}
		resetTimer(timer, s.cfg.ReadyTimeout)
		return StageReady, nil
	case StageReady:
		if state.readyCount() < s.cfg.Expected {
			return stage, nil
		}
		nowNS, err := monotonic.NowNS()
		if err != nil {
			return stage, fmt.Errorf("clock_gettime(CLOCK_MONOTONIC): %w", err)
		}
		state.schedule = ScheduleMessage{
			Type:         TypeSchedule,
			StartAtNS:    nowNS + s.cfg.Warmup.Nanoseconds(),
			StopAtNS:     nowNS + s.cfg.Warmup.Nanoseconds() + s.cfg.Duration.Nanoseconds(),
			DrainUntilNS: nowNS + s.cfg.Warmup.Nanoseconds() + s.cfg.Duration.Nanoseconds() + s.cfg.Drain.Nanoseconds(),
		}
		for _, p := range state.sortedParticipants() {
			if err := s.sendSchedule(p.ConnID, state.schedule); err != nil {
				return stage, err
			}
		}
		state.scheduleSent = true
		resetTimer(timer, s.cfg.AckTimeout)
		return StageAck, nil
	case StageAck:
		if state.ackCount() < s.cfg.Expected {
			return stage, nil
		}
		resetTimer(timer, s.cfg.DoneTimeout)
		return StageDone, nil
	case StageDone:
		if state.doneCount() < s.cfg.Expected {
			return stage, nil
		}
		return "", nil
	default:
		return stage, fmt.Errorf("unknown control stage %q", stage)
	}
}

func (s *Server) acceptLoop(ctx context.Context, events chan<- event) {
	for {
		conn, err := s.ln.Accept()
		if err != nil {
			if ctx.Err() != nil || errors.Is(err, net.ErrClosed) {
				return
			}
			sendEvent(ctx, events, event{err: fmt.Errorf("accept control connection: %w", err)})
			return
		}
		id := s.addConn(conn)
		go s.readConn(ctx, id, conn, events)
	}
}

func (s *Server) addConn(conn net.Conn) int {
	s.mu.Lock()
	defer s.mu.Unlock()
	id := s.nextID
	s.nextID++
	s.conns[id] = conn
	return id
}

func (s *Server) readConn(ctx context.Context, connID int, conn net.Conn, events chan<- event) {
	dec := json.NewDecoder(conn)
	for {
		var raw json.RawMessage
		if err := dec.Decode(&raw); err != nil {
			if ctx.Err() != nil || errors.Is(err, io.EOF) {
				return
			}
			sendEvent(ctx, events, event{err: fmt.Errorf("decode control message: %w", err)})
			return
		}
		ev, err := decodeEvent(connID, raw)
		if err != nil {
			sendEvent(ctx, events, event{err: err})
			return
		}
		sendEvent(ctx, events, ev)
	}
}

func (s *Server) sendSchedule(connID int, schedule ScheduleMessage) error {
	s.mu.Lock()
	conn := s.conns[connID]
	s.mu.Unlock()
	if conn == nil {
		return fmt.Errorf("send schedule: connection %d is gone", connID)
	}
	if err := conn.SetWriteDeadline(time.Now().Add(5 * time.Second)); err != nil {
		return fmt.Errorf("set control write deadline: %w", err)
	}
	if err := json.NewEncoder(conn).Encode(schedule); err != nil {
		return fmt.Errorf("send schedule to connection %d: %w", connID, err)
	}
	return nil
}

type eventKind int

const (
	eventHello eventKind = iota + 1
	eventReady
	eventAck
	eventDone
)

type event struct {
	connID int
	kind   eventKind
	hello  HelloMessage
	ready  ReadyMessage
	ack    SchedAckMessage
	done   DoneMessage
	err    error
}

func decodeEvent(connID int, raw json.RawMessage) (event, error) {
	var env struct {
		Type string `json:"type"`
	}
	if err := json.Unmarshal(raw, &env); err != nil {
		return event{}, fmt.Errorf("decode control envelope: %w", err)
	}
	switch env.Type {
	case TypeHello:
		var msg HelloMessage
		if err := json.Unmarshal(raw, &msg); err != nil {
			return event{}, fmt.Errorf("decode hello: %w", err)
		}
		return event{connID: connID, kind: eventHello, hello: msg}, nil
	case TypeReady:
		var msg ReadyMessage
		if err := json.Unmarshal(raw, &msg); err != nil {
			return event{}, fmt.Errorf("decode ready: %w", err)
		}
		return event{connID: connID, kind: eventReady, ready: msg}, nil
	case TypeSchedAck:
		var msg SchedAckMessage
		if err := json.Unmarshal(raw, &msg); err != nil {
			return event{}, fmt.Errorf("decode sched_ack: %w", err)
		}
		return event{connID: connID, kind: eventAck, ack: msg}, nil
	case TypeDone:
		var msg DoneMessage
		if err := json.Unmarshal(raw, &msg); err != nil {
			return event{}, fmt.Errorf("decode done: %w", err)
		}
		if len(msg.Stats) == 0 {
			msg.Stats = json.RawMessage(`{}`)
		}
		return event{connID: connID, kind: eventDone, done: msg}, nil
	default:
		return event{}, fmt.Errorf("unknown control message type %q", env.Type)
	}
}

func sendEvent(ctx context.Context, events chan<- event, ev event) {
	select {
	case events <- ev:
	case <-ctx.Done():
	}
}

type runState struct {
	expected     int
	participants map[int]*Participant
	procIndex    map[string]int
	invalid      []string
	schedule     ScheduleMessage
	scheduleSent bool
}

func newRunState(expected int) *runState {
	return &runState{
		expected:     expected,
		participants: make(map[int]*Participant),
		procIndex:    make(map[string]int),
	}
}

func (s *runState) apply(ev event) error {
	switch ev.kind {
	case eventHello:
		if _, ok := s.participants[ev.connID]; ok {
			return fmt.Errorf("duplicate hello on connection %d", ev.connID)
		}
		if len(s.participants) >= s.expected {
			return fmt.Errorf("received more than %d hello messages", s.expected)
		}
		if ev.hello.ProcIndex >= 0 {
			// proc_index の一意性は role 内でのみ要求する
			// (server と client 0 は同じ proc_index 0 を名乗る)
			key := fmt.Sprintf("%s#%d", ev.hello.Role, ev.hello.ProcIndex)
			if other, ok := s.procIndex[key]; ok {
				return fmt.Errorf("duplicate %s proc_index %d on connections %d and %d", ev.hello.Role, ev.hello.ProcIndex, other, ev.connID)
			}
			s.procIndex[key] = ev.connID
		}
		s.participants[ev.connID] = &Participant{ConnID: ev.connID, Hello: ev.hello}
	case eventReady:
		p := s.participants[ev.connID]
		if p == nil {
			return fmt.Errorf("ready before hello on connection %d", ev.connID)
		}
		if p.ReadyReceived {
			return fmt.Errorf("duplicate ready on connection %d", ev.connID)
		}
		p.Ready = ev.ready
		p.ReadyReceived = true
	case eventAck:
		p := s.participants[ev.connID]
		if p == nil {
			return fmt.Errorf("sched_ack before hello on connection %d", ev.connID)
		}
		if !p.ReadyReceived {
			return fmt.Errorf("sched_ack before ready on connection %d", ev.connID)
		}
		if !s.scheduleSent {
			return fmt.Errorf("sched_ack before schedule on connection %d", ev.connID)
		}
		if p.AckReceived {
			return fmt.Errorf("duplicate sched_ack on connection %d", ev.connID)
		}
		p.SchedAck = ev.ack
		p.AckReceived = true
		if ev.ack.MarginNS < 0 {
			s.invalid = append(s.invalid, fmt.Sprintf("proc_index=%d pid=%d negative schedule margin: %d ns", p.Hello.ProcIndex, p.Hello.PID, ev.ack.MarginNS))
		}
	case eventDone:
		p := s.participants[ev.connID]
		if p == nil {
			return fmt.Errorf("done before hello on connection %d", ev.connID)
		}
		if !p.AckReceived {
			return fmt.Errorf("done before sched_ack on connection %d", ev.connID)
		}
		if p.DoneReceived {
			return fmt.Errorf("duplicate done on connection %d", ev.connID)
		}
		p.Done = ev.done
		p.DoneReceived = true
	default:
		return fmt.Errorf("unknown event kind %d", ev.kind)
	}
	return nil
}

func (s *runState) helloCount() int {
	return len(s.participants)
}

func (s *runState) readyCount() int {
	n := 0
	for _, p := range s.participants {
		if p.ReadyReceived {
			n++
		}
	}
	return n
}

func (s *runState) ackCount() int {
	n := 0
	for _, p := range s.participants {
		if p.AckReceived {
			n++
		}
	}
	return n
}

func (s *runState) doneCount() int {
	n := 0
	for _, p := range s.participants {
		if p.DoneReceived {
			n++
		}
	}
	return n
}

func (s *runState) sortedParticipants() []Participant {
	out := make([]Participant, 0, len(s.participants))
	for _, p := range s.participants {
		out = append(out, *p)
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].Hello.ProcIndex == out[j].Hello.ProcIndex {
			return out[i].ConnID < out[j].ConnID
		}
		return out[i].Hello.ProcIndex < out[j].Hello.ProcIndex
	})
	return out
}

func (s *runState) result() *Result {
	participants := s.sortedParticipants()
	stats := make([]json.RawMessage, 0, len(participants))
	for _, p := range participants {
		if p.DoneReceived {
			stats = append(stats, append(json.RawMessage(nil), p.Done.Stats...))
		}
	}
	return &Result{
		Valid:          len(s.invalid) == 0,
		InvalidReasons: append([]string(nil), s.invalid...),
		Schedule:       s.schedule,
		Participants:   participants,
		Stats:          stats,
	}
}

func withDefaults(cfg Config) Config {
	if cfg.HelloTimeout == 0 {
		cfg.HelloTimeout = 30 * time.Second
	}
	if cfg.ReadyTimeout == 0 {
		cfg.ReadyTimeout = 30 * time.Second
	}
	if cfg.AckTimeout == 0 {
		cfg.AckTimeout = 30 * time.Second
	}
	if cfg.DoneTimeout == 0 {
		cfg.DoneTimeout = 30 * time.Second
	}
	return cfg
}

func validateConfig(cfg Config) error {
	if cfg.SocketPath == "" {
		return errors.New("control socket path is required")
	}
	if cfg.Expected <= 0 {
		return fmt.Errorf("expected process count must be > 0, got %d", cfg.Expected)
	}
	for name, d := range map[string]time.Duration{
		"warmup":        cfg.Warmup,
		"duration":      cfg.Duration,
		"drain":         cfg.Drain,
		"hello_timeout": cfg.HelloTimeout,
		"ready_timeout": cfg.ReadyTimeout,
		"ack_timeout":   cfg.AckTimeout,
		"done_timeout":  cfg.DoneTimeout,
	} {
		if d < 0 {
			return fmt.Errorf("%s must be >= 0", name)
		}
	}
	return nil
}

func timeoutForStage(cfg Config, stage Stage) time.Duration {
	switch stage {
	case StageHello:
		return cfg.HelloTimeout
	case StageReady:
		return cfg.ReadyTimeout
	case StageAck:
		return cfg.AckTimeout
	case StageDone:
		return cfg.DoneTimeout
	default:
		return 0
	}
}

func resetTimer(timer *time.Timer, d time.Duration) {
	if !timer.Stop() {
		select {
		case <-timer.C:
		default:
		}
	}
	timer.Reset(d)
}
