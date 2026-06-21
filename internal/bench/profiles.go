package bench

import (
	"strconv"
	"strings"
)

// Profile describes a single benchmark traffic shape.
type Profile struct {
	Name        string
	UseCase     string
	Mode        string // "echo" or "broadcast"
	RateR       int
	RateU       int
	Size        int
	Conns       []int
	ClientProcs int
	Notes       string
}

const (
	DefaultLibs              = "mini_rudp,coop_rudp,apex_rudp,enet,kcp,slikenet,raknet,udt4,yojimbo,gns,litenetlib,msquic,quiche,lsquic"
	DefaultMediaConns        = "1 5 50 75 100 125 150 200"
	DefaultGameConns         = "1 5 64 96 128 192 256"
	DefaultEchoConns         = "1 50 200 600 1000 1500 2000 3000"
	DefaultReliableEchoConns = "1 50 200 600 1000 1500 2000 3000"
)

// ProfileFields matches the Python PROFILE_FIELDS list for CSV output.
var ProfileFields = []string{
	"profile",
	"use_case",
	"mode",
	"rate_r",
	"rate_u",
	"size",
	"conns_schedule",
	"client_procs",
	"notes",
}

// CapacityFields matches the Python CAPACITY_FIELDS list for CSV output.
var CapacityFields = []string{
	"profile",
	"library",
	"status",
	"last_ok_conns",
	"last_ok_delivery",
	"last_ok_server_cpu",
	"break_conns",
	"break_reason",
	"break_delivery",
	"break_server_cpu",
}

// ParseInts splits a space-or-comma separated string into a slice of ints.
// An empty string returns an empty (non-nil) slice.
func ParseInts(s string) []int {
	s = strings.ReplaceAll(s, ",", " ")
	out := []int{}
	for _, tok := range strings.Fields(s) {
		v, err := strconv.Atoi(tok)
		if err != nil {
			continue
		}
		out = append(out, v)
	}
	return out
}

// DefaultProfiles returns the 4 canonical profiles with default conn schedules.
func DefaultProfiles() []Profile {
	return []Profile{
		{
			Name:        "media_relay",
			UseCase:     "media_sfu_unreliable_fanout",
			Mode:        "broadcast",
			RateR:       0,
			RateU:       30,
			Size:        1000,
			Conns:       ParseInts(DefaultMediaConns),
			ClientProcs: 4,
			Notes:       "near-MTU media packets, full-room unreliable fanout",
		},
		{
			Name:        "game_server",
			UseCase:     "authoritative_game_snapshot_event_fanout",
			Mode:        "broadcast",
			RateR:       1,
			RateU:       20,
			Size:        128,
			Conns:       ParseInts(DefaultGameConns),
			ClientProcs: 4,
			Notes:       "20Hz state/input fanout plus 1Hz reliable gameplay events",
		},
		{
			Name:        "reliable_echo",
			UseCase:     "reliable_transport_echo_baseline",
			Mode:        "echo",
			RateR:       50,
			RateU:       0,
			Size:        64,
			Conns:       ParseInts(DefaultReliableEchoConns),
			ClientProcs: 8,
			Notes:       "reliable-only echo baseline for stream/reliable transports",
		},
		{
			Name:        "echo",
			UseCase:     "synthetic_mixed_echo_baseline",
			Mode:        "echo",
			RateR:       50,
			RateU:       50,
			Size:        64,
			Conns:       ParseInts(DefaultEchoConns),
			ClientProcs: 8,
			Notes:       "mixed 50/50 echo baseline used for implementation validation",
		},
	}
}

// ProfileByName looks up a profile by name from the default set.
func ProfileByName(name string) (Profile, bool) {
	for _, p := range DefaultProfiles() {
		if p.Name == name {
			return p, true
		}
	}
	return Profile{}, false
}
