package control

import "encoding/json"

const (
	TypeHello    = "hello"
	TypeReady    = "ready"
	TypeSchedule = "schedule"
	TypeSchedAck = "sched_ack"
	TypeDone     = "done"
	// benchspec v2(定常判定つき warmup): schedule は暫定窓(start_at =
	// now + warmup 上限)として配布され、client は受信直後から送信を開始し
	// rate を周期報告する。orchestrator が全 client の送受レート定常を検出
	// したら window で確定窓を配布する(全参加者が window_ack を返す)。
	// 上限まで定常に達しない場合 window は送られず、暫定窓がそのまま有効。
	TypeRate      = "rate"
	TypeWindow    = "window"
	TypeWindowAck = "window_ack"
)

type HelloMessage struct {
	Type      string `json:"type"`
	Role      string `json:"role"`
	Transport string `json:"transport,omitempty"`
	Lib       string `json:"lib,omitempty"`
	PID       int    `json:"pid"`
	ProcIndex int    `json:"proc_index"`
}

type ReadyMessage struct {
	Type  string `json:"type"`
	Conns int    `json:"conns"`
}

type ScheduleMessage struct {
	Type         string `json:"type"`
	StartAtNS    int64  `json:"start_at_ns"`
	StopAtNS     int64  `json:"stop_at_ns"`
	DrainUntilNS int64  `json:"drain_until_ns"`
}

type SchedAckMessage struct {
	Type     string `json:"type"`
	MarginNS int64  `json:"margin_ns"`
}

// RateMessage は client の周期レート報告(累積カウンタの生値)。
type RateMessage struct {
	Type     string `json:"type"`
	Sent     uint64 `json:"sent"`     // raw submitted(計測 bit 無関係の累積)
	Received uint64 `json:"received"` // raw 受信(measured + unmeasured の累積)
}

// WindowMessage は定常検出後の確定計測窓。フィールドは schedule と同形。
type WindowMessage struct {
	Type         string `json:"type"`
	StartAtNS    int64  `json:"start_at_ns"`
	StopAtNS     int64  `json:"stop_at_ns"`
	DrainUntilNS int64  `json:"drain_until_ns"`
}

type WindowAckMessage struct {
	Type     string `json:"type"`
	MarginNS int64  `json:"margin_ns"`
}

type DoneMessage struct {
	Type  string          `json:"type"`
	Stats json.RawMessage `json:"stats"`
}

type Participant struct {
	ConnID        int             `json:"conn_id"`
	Hello         HelloMessage    `json:"hello"`
	Ready         ReadyMessage    `json:"ready"`
	ReadyReceived bool            `json:"ready_received"`
	SchedAck      SchedAckMessage `json:"sched_ack"`
	AckReceived   bool            `json:"ack_received"`
	Done          DoneMessage     `json:"done"`
	DoneReceived  bool            `json:"done_received"`
	// 定常判定つき warmup の開示: 受信した rate 報告数と直近の増分
	// (定常が成立しなかった run の診断用)
	RateReports int      `json:"rate_reports,omitempty"`
	RateDSent   []uint64 `json:"rate_dsent,omitempty"`
	RateDRecv   []uint64 `json:"rate_drecv,omitempty"`
}

type Result struct {
	Valid          bool              `json:"valid"`
	InvalidReasons []string          `json:"invalid_reasons,omitempty"`
	Schedule       ScheduleMessage   `json:"schedule"`
	Participants   []Participant     `json:"participants"`
	Stats          []json.RawMessage `json:"stats,omitempty"`
	// 定常判定つき warmup(benchspec v2)の結果開示。
	// SteadyReached=false かつ steady 有効時は暫定窓(上限 warmup)で計測した。
	SteadyEnabled  bool  `json:"steady_enabled,omitempty"`
	SteadyReached  bool  `json:"steady_reached,omitempty"`
	WarmupActualNS int64 `json:"warmup_actual_ns,omitempty"`
}
