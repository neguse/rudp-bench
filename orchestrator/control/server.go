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
	// StageWindow: 定常判定つき warmup(benchspec v2)。client の rate 報告から
	// 送受レートの定常を検出し、確定窓(window)を配布する。上限(cfg.Warmup)
	// までに定常に達しなければ暫定窓のまま計測に入る(エラーではない)。
	StageWindow    Stage = "window"
	StageWindowAck Stage = "window_ack"
	StageDone      Stage = "done"
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

	// Warmup: SteadyWarmup=false のときは固定 warmup。true のときは warmup の
	// **上限**(暫定窓の start_at = ready + Warmup)で、定常検出が先に成立すれば
	// window でそれより早く計測窓が確定する。
	Warmup   time.Duration
	Duration time.Duration
	Drain    time.Duration

	// SteadyWarmup: 定常判定つき warmup(benchspec v2)を有効化する。
	// client は schedule 受信直後から送信を開始し rate を周期報告する契約。
	SteadyWarmup bool
	// 定常判定は「窓の比較」で行う: 直近 SteadyBuckets 個の報告増分の合計
	// (W2)と、その直前 SteadyBuckets 個の合計(W1)が送受とも
	// ± SteadyTolPct% 以内なら flat。バケット単体の比較にしないのは、
	// 送信 slot 境界と報告周期のエイリアシングでバケット値が量子化振動する
	// ため(1秒窓なら量子化は数%に落ちる)。
	// さらに SteadyExpected*PerConn > 0 のとき、W2 が期待レートの 9 割以上で
	// あることも要求する。flat だけだと「劣化したまま安定」(enet throttle が
	// 低位で整定した状態)を定常と誤認し、二峰性が残るため。
	SteadyBuckets int           // 既定 4(報告 250ms × 4 = 1秒窓)
	SteadyTolPct  float64       // 既定 10
	SteadyGuard   time.Duration // window 配布から窓開始までの余裕。既定 500ms
	// SteadyMinWarmup: 定常が検出されてもこの時間より前に窓を開かない。
	// レート形状からは予測できない遅い過渡(enet の throttle は接続ストーム
	// 後 ~13s、レートが数秒 flat に見えた後で崩れることを 20 反復×2 系列で
	// 実測)を持つ transport 向けの宣言値。0 = 制約なし
	SteadyMinWarmup time.Duration
	// 期待レート(per conn / per second、loss 調整済み)。0 なら下限チェック無効
	SteadyExpectedSentPerConn float64
	SteadyExpectedRecvPerConn float64

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

	serverReadyOnce sync.Once
	serverReadyCh   chan struct{}
}

// ServerReady は role=server の参加者から ready を受信した時点で close される。
// runner は server の listen 完了を待ってから client 群を起動する
// (起動が遅い server に client が先に接続を試みて即死するレースの防止)。
func (s *Server) ServerReady() <-chan struct{} {
	return s.serverReadyCh
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
	// sudo 実行時、権限降下したベンチマークプロセスからも接続できるようにする
	if err := os.Chmod(cfg.SocketPath, 0o666); err != nil {
		_ = ln.Close()
		return nil, fmt.Errorf("chmod control socket: %w", err)
	}
	return &Server{
		cfg:           cfg,
		ln:            ln,
		removeSocket:  true,
		conns:         make(map[int]net.Conn),
		serverReadyCh: make(chan struct{}),
	}, nil
}

func newServerWithListener(cfg Config, ln net.Listener) (*Server, error) {
	cfg = withDefaults(cfg)
	if err := validateConfig(cfg); err != nil {
		return nil, err
	}
	return &Server{
		cfg:           cfg,
		ln:            ln,
		conns:         make(map[int]net.Conn),
		serverReadyCh: make(chan struct{}),
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
	state.steadyEnabled = s.cfg.SteadyWarmup
	stage := StageHello
	timer := time.NewTimer(s.cfg.HelloTimeout)
	defer timer.Stop()

	for {
		select {
		case <-ctx.Done():
			return state.result(), ctx.Err()
		case <-timer.C:
			// StageWindow のタイマ満了は「定常未達のまま warmup 上限に到達」で
			// あってエラーではない。暫定窓のまま計測に入る(結果に開示される)。
			if stage == StageWindow {
				state.steadyReached = false
				resetTimer(timer, s.cfg.DoneTimeout)
				stage = StageDone
				continue
			}
			return state.result(), TimeoutError{Stage: stage, Timeout: timeoutForStage(s.cfg, stage)}
		case ev := <-events:
			if ev.err != nil {
				return state.result(), ev.err
			}
			if err := state.apply(ev); err != nil {
				return state.result(), err
			}
			if ev.kind == eventReady {
				if p := state.participants[ev.connID]; p != nil && p.Hello.Role == "server" {
					s.serverReadyOnce.Do(func() { close(s.serverReadyCh) })
				}
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
		// SteadyWarmup 時、この schedule は「暫定窓」(start_at = warmup 上限)。
		// 定常が先に成立すれば window で上書きされる
		state.scheduleIssuedNS = nowNS
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
		if !s.cfg.SteadyWarmup {
			resetTimer(timer, s.cfg.DoneTimeout)
			return StageDone, nil
		}
		// 定常判定フェーズへ。タイマは「window を発行できる最終時刻」
		// (暫定 start_at − guard)に合わせる。満了 = 定常未達で暫定窓に committed
		nowNS, err := monotonic.NowNS()
		if err != nil {
			return stage, fmt.Errorf("clock_gettime(CLOCK_MONOTONIC): %w", err)
		}
		cutoff := time.Duration(state.schedule.StartAtNS-nowNS)*time.Nanosecond - s.steadyGuard()
		if cutoff < 0 {
			cutoff = 0
		}
		resetTimer(timer, cutoff)
		return StageWindow, nil
	case StageWindow:
		if !state.steadyDetected(s.cfg) {
			return stage, nil
		}
		nowNS, err := monotonic.NowNS()
		if err != nil {
			return stage, fmt.Errorf("clock_gettime(CLOCK_MONOTONIC): %w", err)
		}
		// 宣言された最小 warmup より前には開かない(定常は必要条件であって
		// 十分条件ではない transport がある — Config.SteadyMinWarmup 参照)
		if s.cfg.SteadyMinWarmup > 0 &&
			nowNS < state.scheduleIssuedNS+s.cfg.SteadyMinWarmup.Nanoseconds() {
			return stage, nil
		}
		startNS := nowNS + s.steadyGuard().Nanoseconds()
		if startNS >= state.schedule.StartAtNS {
			// 上限直前に定常が成立しても暫定窓のまま行く(窓の前倒しにならない)
			state.steadyReached = false
			resetTimer(timer, s.cfg.DoneTimeout)
			return StageDone, nil
		}
		window := WindowMessage{
			Type:         TypeWindow,
			StartAtNS:    startNS,
			StopAtNS:     startNS + s.cfg.Duration.Nanoseconds(),
			DrainUntilNS: startNS + s.cfg.Duration.Nanoseconds() + s.cfg.Drain.Nanoseconds(),
		}
		for _, p := range state.sortedParticipants() {
			if err := s.sendWindow(p.ConnID, window); err != nil {
				return stage, err
			}
		}
		// 以降の実効窓は window(result の schedule にも反映する)
		state.schedule.StartAtNS = window.StartAtNS
		state.schedule.StopAtNS = window.StopAtNS
		state.schedule.DrainUntilNS = window.DrainUntilNS
		state.windowSent = true
		state.steadyReached = true
		resetTimer(timer, s.cfg.AckTimeout)
		return StageWindowAck, nil
	case StageWindowAck:
		if state.windowAckCount() < s.cfg.Expected {
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
	return s.sendJSON(connID, "schedule", schedule)
}

func (s *Server) sendWindow(connID int, window WindowMessage) error {
	return s.sendJSON(connID, "window", window)
}

func (s *Server) sendJSON(connID int, what string, msg any) error {
	s.mu.Lock()
	conn := s.conns[connID]
	s.mu.Unlock()
	if conn == nil {
		return fmt.Errorf("send %s: connection %d is gone", what, connID)
	}
	if err := conn.SetWriteDeadline(time.Now().Add(5 * time.Second)); err != nil {
		return fmt.Errorf("set control write deadline: %w", err)
	}
	if err := json.NewEncoder(conn).Encode(msg); err != nil {
		return fmt.Errorf("send %s to connection %d: %w", what, connID, err)
	}
	return nil
}

func (s *Server) steadyGuard() time.Duration {
	if s.cfg.SteadyGuard > 0 {
		return s.cfg.SteadyGuard
	}
	return 500 * time.Millisecond
}

func (s *Server) steadyBuckets() int {
	if s.cfg.SteadyBuckets > 0 {
		return s.cfg.SteadyBuckets
	}
	return 4
}

type eventKind int

const (
	eventHello eventKind = iota + 1
	eventReady
	eventAck
	eventRate
	eventWindowAck
	eventDone
)

type event struct {
	connID    int
	kind      eventKind
	hello     HelloMessage
	ready     ReadyMessage
	ack       SchedAckMessage
	rate      RateMessage
	windowAck WindowAckMessage
	done      DoneMessage
	err       error
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
	case TypeRate:
		var msg RateMessage
		if err := json.Unmarshal(raw, &msg); err != nil {
			return event{}, fmt.Errorf("decode rate: %w", err)
		}
		return event{connID: connID, kind: eventRate, rate: msg}, nil
	case TypeWindowAck:
		var msg WindowAckMessage
		if err := json.Unmarshal(raw, &msg); err != nil {
			return event{}, fmt.Errorf("decode window_ack: %w", err)
		}
		return event{connID: connID, kind: eventWindowAck, windowAck: msg}, nil
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

	// 定常判定つき warmup(benchspec v2)の実行時状態
	scheduleIssuedNS int64
	windowSent       bool
	steadyEnabled    bool
	steadyReached    bool
	rates            map[int]*rateTrack // connID → client の rate 履歴
	windowAcked      map[int]bool
}

// rateTrack は client 1 プロセスの rate 報告履歴(直近の増分のみ保持)。
type rateTrack struct {
	last    RateMessage
	hasLast bool
	dSent   []uint64
	dRecv   []uint64
}

func newRunState(expected int) *runState {
	return &runState{
		expected:     expected,
		participants: make(map[int]*Participant),
		procIndex:    make(map[string]int),
		rates:        make(map[int]*rateTrack),
		windowAcked:  make(map[int]bool),
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
	case eventRate:
		p := s.participants[ev.connID]
		if p == nil {
			return fmt.Errorf("rate before hello on connection %d", ev.connID)
		}
		if p.Hello.Role != "client" {
			return fmt.Errorf("rate from non-client role %q on connection %d", p.Hello.Role, ev.connID)
		}
		t := s.rates[ev.connID]
		if t == nil {
			t = &rateTrack{}
			s.rates[ev.connID] = t
		}
		if t.hasLast {
			// 累積カウンタは単調増加の契約。巻き戻りは 0 増分として扱う
			var dSent, dRecv uint64
			if ev.rate.Sent >= t.last.Sent {
				dSent = ev.rate.Sent - t.last.Sent
			}
			if ev.rate.Received >= t.last.Received {
				dRecv = ev.rate.Received - t.last.Received
			}
			t.dSent = append(t.dSent, dSent)
			t.dRecv = append(t.dRecv, dRecv)
			const keep = 32
			if len(t.dSent) > keep {
				t.dSent = t.dSent[len(t.dSent)-keep:]
				t.dRecv = t.dRecv[len(t.dRecv)-keep:]
			}
		}
		t.last = ev.rate
		t.hasLast = true
		p.RateReports++
	case eventWindowAck:
		p := s.participants[ev.connID]
		if p == nil {
			return fmt.Errorf("window_ack before hello on connection %d", ev.connID)
		}
		if s.windowAcked[ev.connID] {
			return fmt.Errorf("duplicate window_ack on connection %d", ev.connID)
		}
		s.windowAcked[ev.connID] = true
		if ev.windowAck.MarginNS < 0 {
			s.invalid = append(s.invalid, fmt.Sprintf("proc_index=%d pid=%d negative window margin: %d ns", p.Hello.ProcIndex, p.Hello.PID, ev.windowAck.MarginNS))
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

// steadyBucketSec は client の rate 報告周期(benchspec v2 で 250ms 固定)。
const steadyBucketSec = 0.25

// steadyDetected: 全 client が以下を満たしたら定常。
//   - flat: 直近 K 個の増分の合計 W2 と、その前 K 個の合計 W1 が送受とも
//     ±tol% 以内(かつ W2 > 0)。バケット単体比較にしないのは、送信 slot
//     境界と報告周期のエイリアシングでバケット値が量子化振動するため
//     (例: 13 conns × 21pps の 250ms バケットは 52/65/78/91 を往復する)
//   - floor(期待レート設定時): W2 が期待レート × 0.9 以上。flat だけだと
//     「劣化したまま安定」(enet throttle が低位で整定)を定常と誤認する
//
// client 数は expected から server 1 を引いた数とみなす
// (v2 の run は常に server 1 + client N)。
func (s *runState) steadyDetected(cfg Config) bool {
	buckets := cfg.SteadyBuckets
	if buckets <= 0 {
		buckets = 4
	}
	tolPct := cfg.SteadyTolPct
	if tolPct <= 0 {
		tolPct = 10
	}
	clients := 0
	for _, p := range s.participants {
		if p.Hello.Role == "client" {
			clients++
		}
	}
	if clients == 0 || clients != cfg.Expected-1 {
		return false
	}
	windowSec := float64(buckets) * steadyBucketSec
	for connID, p := range s.participants {
		if p.Hello.Role != "client" {
			continue
		}
		t := s.rates[connID]
		if t == nil || len(t.dSent) < 2*buckets {
			return false
		}
		sent2 := sumTail(t.dSent, buckets, 0)
		sent1 := sumTail(t.dSent, buckets, buckets)
		recv2 := sumTail(t.dRecv, buckets, 0)
		recv1 := sumTail(t.dRecv, buckets, buckets)
		if !flatWindows(sent1, sent2, tolPct) || !flatWindows(recv1, recv2, tolPct) {
			return false
		}
		conns := float64(p.Ready.Conns)
		if cfg.SteadyExpectedSentPerConn > 0 &&
			float64(sent2) < 0.9*cfg.SteadyExpectedSentPerConn*conns*windowSec {
			return false
		}
		if cfg.SteadyExpectedRecvPerConn > 0 &&
			float64(recv2) < 0.9*cfg.SteadyExpectedRecvPerConn*conns*windowSec {
			return false
		}
	}
	return true
}

// sumTail は末尾から back 個ずらした位置の n 個の合計。
func sumTail(d []uint64, n, back int) uint64 {
	var sum uint64
	end := len(d) - back
	for i := end - n; i < end; i++ {
		sum += d[i]
	}
	return sum
}

// flatWindows: 連続する2窓の合計が ±tol% 以内で、直近窓が正であること。
func flatWindows(w1, w2 uint64, tolPct float64) bool {
	if w2 == 0 {
		return false
	}
	hi := float64(w1)
	lo := float64(w2)
	if lo > hi {
		hi, lo = lo, hi
	}
	return hi-lo <= hi*tolPct/100
}

func (s *runState) windowAckCount() int {
	return len(s.windowAcked)
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
	for connID, p := range s.participants {
		cp := *p
		if t := s.rates[connID]; t != nil {
			cp.RateDSent = append([]uint64(nil), t.dSent...)
			cp.RateDRecv = append([]uint64(nil), t.dRecv...)
		}
		out = append(out, cp)
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
	r := &Result{
		Valid:          len(s.invalid) == 0,
		InvalidReasons: append([]string(nil), s.invalid...),
		Schedule:       s.schedule,
		Participants:   participants,
		Stats:          stats,
		SteadyEnabled:  s.steadyEnabled,
		SteadyReached:  s.steadyReached,
	}
	if s.scheduleIssuedNS > 0 && s.schedule.StartAtNS > s.scheduleIssuedNS {
		r.WarmupActualNS = s.schedule.StartAtNS - s.scheduleIssuedNS
	}
	return r
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
	case StageAck, StageWindowAck:
		return cfg.AckTimeout
	case StageWindow:
		return cfg.Warmup
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
