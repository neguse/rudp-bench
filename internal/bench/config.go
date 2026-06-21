package bench

import (
	"encoding/json"
	"os"
	"strings"
)

// Scenario describes a benchmark scenario loaded from a JSON configuration file.
type Scenario struct {
	Locked      bool     `json:"locked"`
	Libs        []string `json:"libs"`
	Profiles    []string `json:"profiles"`
	Runs        int      `json:"runs"`
	Duration    int      `json:"duration"`
	TailMs      int      `json:"tail_ms"`
	Netem       bool     `json:"netem"`
	NetemArgs   string   `json:"netem_args"`
	Isolate     string   `json:"isolate"`
	ServerCPU   string   `json:"server_cpu"`
	ClientCPU   string   `json:"client_cpu"`
	MinValid    int      `json:"min_valid"`
	MinDelivery float64  `json:"min_delivery"`
	Build       bool     `json:"build"`
	Publish     bool     `json:"publish"`
	// Optional per-profile conns override
	MediaConns        string `json:"media_conns,omitempty"`
	GameConns         string `json:"game_conns,omitempty"`
	EchoConns         string `json:"echo_conns,omitempty"`
	ReliableEchoConns string `json:"reliable_echo_conns,omitempty"`
}

// Config holds runtime state for a benchmark invocation.
type Config struct {
	Root      string
	CMake     string
	BuildDir  string
	Out       string
	Jobs      int
	DryRun    bool
	Plan      bool
	Resume    bool
	NoBuild   bool
	NoPublish bool
}

// LoadScenario reads a JSON file at path and returns a Scenario with defaults
// applied for zero-value fields.
func LoadScenario(path string) (*Scenario, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	s := &Scenario{}
	if err := json.Unmarshal(data, s); err != nil {
		return nil, err
	}
	applyDefaults(s)
	return s, nil
}

// DefaultScenario returns a single-run dev-like scenario with sensible defaults.
func DefaultScenario() *Scenario {
	s := &Scenario{}
	applyDefaults(s)
	return s
}

func applyDefaults(s *Scenario) {
	if s.Duration == 0 {
		s.Duration = 5
	}
	if s.TailMs == 0 {
		s.TailMs = 500
	}
	if s.Runs == 0 {
		s.Runs = 1
	}
	if s.MinDelivery == 0 {
		s.MinDelivery = 0.95
	}
	if s.Isolate == "" {
		s.Isolate = "taskset"
	}
	if len(s.Libs) == 0 {
		s.Libs = strings.Split(DefaultLibs, ",")
	}
}
