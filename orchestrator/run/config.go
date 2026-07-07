package run

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/netops"
)

const (
	defaultAttemptedThreshold = 0.99
	defaultControlTimeout     = 30 * time.Second
)

type Duration struct {
	time.Duration
}

func (d *Duration) UnmarshalJSON(data []byte) error {
	data = bytes.TrimSpace(data)
	if bytes.Equal(data, []byte("null")) {
		d.Duration = 0
		return nil
	}
	if len(data) > 0 && data[0] == '"' {
		var s string
		if err := json.Unmarshal(data, &s); err != nil {
			return err
		}
		v, err := time.ParseDuration(s)
		if err != nil {
			return err
		}
		d.Duration = v
		return nil
	}
	var ns int64
	if err := json.Unmarshal(data, &ns); err != nil {
		return err
	}
	d.Duration = time.Duration(ns)
	return nil
}

func (d Duration) MarshalJSON() ([]byte, error) {
	return json.Marshal(d.Duration.String())
}

type CommandConfig struct {
	Path string   `json:"path"`
	Args []string `json:"args,omitempty"`
	Env  []string `json:"env,omitempty"`
	Dir  string   `json:"dir,omitempty"`
}

func (c *CommandConfig) UnmarshalJSON(data []byte) error {
	data = bytes.TrimSpace(data)
	if len(data) == 0 || bytes.Equal(data, []byte("null")) {
		return nil
	}
	switch data[0] {
	case '{':
		type alias CommandConfig
		var a alias
		if err := json.Unmarshal(data, &a); err != nil {
			return err
		}
		*c = CommandConfig(a)
		return nil
	case '[':
		var argv []string
		if err := json.Unmarshal(data, &argv); err != nil {
			return err
		}
		if len(argv) > 0 {
			c.Path = argv[0]
			c.Args = append([]string(nil), argv[1:]...)
		}
		return nil
	case '"':
		var line string
		if err := json.Unmarshal(data, &line); err != nil {
			return err
		}
		argv := strings.Fields(line)
		if len(argv) > 0 {
			c.Path = argv[0]
			c.Args = append([]string(nil), argv[1:]...)
		}
		return nil
	default:
		return fmt.Errorf("command must be an object, array, or string")
	}
}

type RunConfig struct {
	Transport string `json:"transport"`
	// Workload は docs/profiles.md のセル名(r{pps}p{bytes} / echo /
	// reliable_echo)。指定時は orchestrator が共通フラグ(--rate-* /
	// --payload-* / --broadcast-*)を client_command に付与し、duration
	// 未指定なら loss イベント数規則から自動導出する。
	Workload           string        `json:"workload,omitempty"`
	ServerCommand      CommandConfig `json:"server_command"`
	ClientCommand      CommandConfig `json:"client_command"`
	ClientProcs        int           `json:"client_procs"`
	TotalConns         int           `json:"total_conns"`
	Warmup             Duration      `json:"warmup"`
	// SteadyWarmup: 定常判定つき warmup(benchspec v2)。true のとき Warmup は
	// 上限になり、全 client の送受レート定常を検出した時点で計測窓が確定する。
	// 接続ストーム直後の非定常区間が計測窓に入る二峰性(ledger #14)への対処
	SteadyWarmup bool `json:"steady_warmup,omitempty"`
	// SteadyMinWarmup: 定常が見えてもこれより早く窓を開かない。レート形状から
	// 予測できない遅い過渡を持つ transport(enet throttle ~13s)の宣言値
	SteadyMinWarmup Duration `json:"steady_min_warmup,omitempty"`
	Duration        Duration `json:"duration"`
	Drain           Duration `json:"drain"`
	DeadlineNS         uint64        `json:"deadline_ns"`
	StalenessPeriodNS  uint64        `json:"staleness_period_ns"`
	Netem              *NetemRegime  `json:"netem,omitempty"`
	NetemGateOff       bool          `json:"netem_gate_off,omitempty"`
	// 役割別 CPU 割当(taskset -c 形式)。v1 の役割隔離レイアウトを流用:
	// OS/background 0-2,8-10 / client 3-6,11-14 / server 7,15。
	// OS 側 slice の退避は `orchestrator isolate setup` で行う
	ServerCPUs string `json:"server_cpus,omitempty"`
	ClientCPUs string `json:"client_cpus,omitempty"`
	// TCP 系(blocking send)では client の送信スケジュール遅延は transport の
	// HoL/backpressure そのもの(測定対象)であり、farm 帰属フィルタを適用しない
	SchedIsMeasurand bool `json:"sched_is_measurand,omitempty"`
	OutputDir          string        `json:"output_dir"`
	AttemptedThreshold float64       `json:"attempted_threshold,omitempty"`
	ControlTimeout     Duration      `json:"control_timeout,omitempty"`
	SamplerInterval    Duration      `json:"sampler_interval,omitempty"`
	ProcessExitTimeout Duration      `json:"process_exit_timeout,omitempty"`
}

type NetemRegime struct {
	Prefix         string       `json:"prefix,omitempty"`
	ServerNS       string       `json:"server_ns,omitempty"`
	ClientNS       string       `json:"client_ns,omitempty"`
	ServerVeth     string       `json:"server_veth,omitempty"`
	ClientVeth     string       `json:"client_veth,omitempty"`
	ServerAddrCIDR string       `json:"server_addr_cidr,omitempty"`
	ClientAddrCIDR string       `json:"client_addr_cidr,omitempty"`
	ServerEgress   netops.Netem `json:"server_egress,omitempty"`
	ClientEgress   netops.Netem `json:"client_egress,omitempty"`
}

func LoadConfig(path string) (RunConfig, error) {
	var cfg RunConfig
	data, err := os.ReadFile(path)
	if err != nil {
		return cfg, err
	}
	if err := json.Unmarshal(data, &cfg); err != nil {
		return cfg, err
	}
	return cfg.Prepare()
}

// Prepare はプログラム生成された config を LoadConfig と同じ経路で確定する
// (workload 展開 → defaults → validation)。
func (cfg RunConfig) Prepare() (RunConfig, error) {
	if err := cfg.applyWorkload(); err != nil {
		return cfg, err
	}
	cfg = cfg.withDefaults()
	return cfg, cfg.validate()
}

// applyWorkload はセル名を解決して client 引数と duration に展開する。
func (cfg *RunConfig) applyWorkload() error {
	if cfg.Workload == "" {
		return nil
	}
	w, ok := LookupWorkload(cfg.Workload)
	if !ok {
		return fmt.Errorf("unknown workload %q (defined: %s)", cfg.Workload, strings.Join(WorkloadNames(), ", "))
	}
	for _, arg := range cfg.ClientCommand.Args {
		if strings.HasPrefix(arg, "--rate-") || strings.HasPrefix(arg, "--payload") || strings.HasPrefix(arg, "--broadcast-") {
			return fmt.Errorf("workload %q conflicts with explicit client arg %q", cfg.Workload, arg)
		}
	}
	cfg.ClientCommand.Args = append(cfg.ClientCommand.Args, w.ClientArgs()...)
	if cfg.Duration.Duration == 0 {
		cfg.Duration.Duration = autoDuration(w, cfg.TotalConns, cfg.Netem)
	}
	return nil
}

func (cfg RunConfig) withDefaults() RunConfig {
	if cfg.AttemptedThreshold == 0 {
		cfg.AttemptedThreshold = defaultAttemptedThreshold
	}
	// 0-conn client proc は起動できない(全 client が --conns > 0 を要求する)
	if cfg.TotalConns > 0 && cfg.ClientProcs > cfg.TotalConns {
		cfg.ClientProcs = cfg.TotalConns
	}
	if cfg.ControlTimeout.Duration == 0 {
		cfg.ControlTimeout.Duration = defaultControlTimeout
	}
	if cfg.ProcessExitTimeout.Duration == 0 {
		cfg.ProcessExitTimeout.Duration = 5 * time.Second
	}
	return cfg
}

func (cfg RunConfig) validate() error {
	var errs []error
	if cfg.Transport == "" {
		errs = append(errs, fmt.Errorf("transport is required"))
	}
	if cfg.ServerCommand.Path == "" {
		errs = append(errs, fmt.Errorf("server_command.path is required"))
	}
	if cfg.ClientCommand.Path == "" {
		errs = append(errs, fmt.Errorf("client_command.path is required"))
	}
	if cfg.ClientProcs <= 0 {
		errs = append(errs, fmt.Errorf("client_procs must be > 0, got %d", cfg.ClientProcs))
	}
	if cfg.TotalConns <= 0 {
		errs = append(errs, fmt.Errorf("total_conns must be > 0, got %d", cfg.TotalConns))
	}
	if cfg.Duration.Duration < 0 || cfg.Warmup.Duration < 0 || cfg.Drain.Duration < 0 {
		errs = append(errs, fmt.Errorf("warmup, duration, and drain must be >= 0"))
	}
	if cfg.OutputDir == "" {
		errs = append(errs, fmt.Errorf("output_dir is required"))
	}
	if cfg.AttemptedThreshold < 0 || cfg.AttemptedThreshold > 1 {
		errs = append(errs, fmt.Errorf("attempted_threshold must be between 0 and 1, got %g", cfg.AttemptedThreshold))
	}
	if cfg.ControlTimeout.Duration < 0 || cfg.SamplerInterval.Duration < 0 || cfg.ProcessExitTimeout.Duration < 0 {
		errs = append(errs, fmt.Errorf("timeouts and sampler_interval must be >= 0"))
	}
	return joinErrors(errs)
}

func (n NetemRegime) pairSpec() netops.PairSpec {
	spec := netops.DefaultPair(n.Prefix)
	if n.ServerNS != "" {
		spec.ServerNS = n.ServerNS
	}
	if n.ClientNS != "" {
		spec.ClientNS = n.ClientNS
	}
	if n.ServerVeth != "" {
		spec.ServerVeth = n.ServerVeth
	}
	if n.ClientVeth != "" {
		spec.ClientVeth = n.ClientVeth
	}
	if n.ServerAddrCIDR != "" {
		spec.ServerAddrCIDR = n.ServerAddrCIDR
	}
	if n.ClientAddrCIDR != "" {
		spec.ClientAddrCIDR = n.ClientAddrCIDR
	}
	spec.ServerEgress = n.ServerEgress
	spec.ClientEgress = n.ClientEgress
	// 決定的 loss 注入(loss_seed)は netns 内で orchestrator 自身を
	// `ip netns exec` するため、実行ファイル path を渡す
	if n.ServerEgress.LossSeed != 0 || n.ClientEgress.LossSeed != 0 {
		if exe, err := os.Executable(); err == nil {
			spec.SelfExe = exe
		}
	}
	return spec
}

type templateVars struct {
	Transport     string
	ProcIndex     int
	Conns         int
	OriginIDStart int
	OriginIDEnd   int
	TotalConns    int
}

func expandCommandTemplate(cmd CommandConfig, vars templateVars) CommandConfig {
	out := CommandConfig{
		Path: expandTemplate(cmd.Path, vars),
		Args: make([]string, len(cmd.Args)),
		Env:  make([]string, len(cmd.Env)),
		Dir:  expandTemplate(cmd.Dir, vars),
	}
	for i := range cmd.Args {
		out.Args[i] = expandTemplate(cmd.Args[i], vars)
	}
	for i := range cmd.Env {
		out.Env[i] = expandTemplate(cmd.Env[i], vars)
	}
	return out
}

func expandTemplate(s string, vars templateVars) string {
	values := map[string]string{
		"transport":       vars.Transport,
		"proc_index":      strconv.Itoa(vars.ProcIndex),
		"conns":           strconv.Itoa(vars.Conns),
		"origin_id_start": strconv.Itoa(vars.OriginIDStart),
		"origin_id_end":   strconv.Itoa(vars.OriginIDEnd),
		"origin_start":    strconv.Itoa(vars.OriginIDStart),
		"origin_end":      strconv.Itoa(vars.OriginIDEnd),
		"origin_id_range": fmt.Sprintf("%d:%d", vars.OriginIDStart, vars.OriginIDEnd),
		"total_conns":     strconv.Itoa(vars.TotalConns),
	}
	replacements := make([]string, 0, len(values)*4)
	for key, value := range values {
		replacements = append(replacements, "{{"+key+"}}", value, "{"+key+"}", value)
	}
	return strings.NewReplacer(replacements...).Replace(s)
}

func splitConns(total, procs int) []int {
	out := make([]int, procs)
	base := total / procs
	rem := total % procs
	for i := range out {
		out[i] = base
		if i < rem {
			out[i]++
		}
	}
	return out
}

func joinErrors(errs []error) error {
	var b strings.Builder
	for _, err := range errs {
		if err == nil {
			continue
		}
		if b.Len() > 0 {
			b.WriteString("; ")
		}
		b.WriteString(err.Error())
	}
	if b.Len() == 0 {
		return nil
	}
	return fmt.Errorf("%s", b.String())
}
