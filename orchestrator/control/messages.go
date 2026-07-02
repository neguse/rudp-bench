package control

import "encoding/json"

const (
	TypeHello    = "hello"
	TypeReady    = "ready"
	TypeSchedule = "schedule"
	TypeSchedAck = "sched_ack"
	TypeDone     = "done"
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
}

type Result struct {
	Valid          bool              `json:"valid"`
	InvalidReasons []string          `json:"invalid_reasons,omitempty"`
	Schedule       ScheduleMessage   `json:"schedule"`
	Participants   []Participant     `json:"participants"`
	Stats          []json.RawMessage `json:"stats,omitempty"`
}
