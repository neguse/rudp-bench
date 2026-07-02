package netops

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"os/exec"
	"strconv"
	"strings"
)

type Command struct {
	Name string
	Args []string
}

func (c Command) String() string {
	parts := make([]string, 0, 1+len(c.Args))
	parts = append(parts, c.Name)
	parts = append(parts, c.Args...)
	for i, p := range parts {
		if strings.ContainsAny(p, " \t\n\"'\\") {
			parts[i] = strconv.Quote(p)
		}
	}
	return strings.Join(parts, " ")
}

type Netem struct {
	DelayMS     int     `json:"delay_ms,omitempty"`
	JitterMS    int     `json:"jitter_ms,omitempty"`
	LossPercent float64 `json:"loss_pct,omitempty"`
	Rate        string  `json:"rate,omitempty"`
	Limit       int     `json:"limit,omitempty"`
}

func (n Netem) Enabled() bool {
	return n.DelayMS != 0 || n.JitterMS != 0 || n.LossPercent != 0 || n.Rate != "" || n.Limit != 0
}

func (n Netem) Args() ([]string, error) {
	if n.DelayMS < 0 {
		return nil, fmt.Errorf("netem delay must be >= 0")
	}
	if n.JitterMS < 0 {
		return nil, fmt.Errorf("netem jitter must be >= 0")
	}
	if n.LossPercent < 0 || n.LossPercent > 100 {
		return nil, fmt.Errorf("netem loss_percent must be between 0 and 100")
	}
	limit := n.Limit
	if limit == 0 {
		limit = 10000
	}
	if limit < 0 {
		return nil, fmt.Errorf("netem limit must be >= 0")
	}
	args := []string{"limit", strconv.Itoa(limit)}
	if n.DelayMS != 0 || n.JitterMS != 0 {
		args = append(args, "delay", fmt.Sprintf("%dms", n.DelayMS))
		if n.JitterMS != 0 {
			args = append(args, fmt.Sprintf("%dms", n.JitterMS), "distribution", "normal")
		}
	}
	if n.LossPercent != 0 {
		args = append(args, "loss", formatPercent(n.LossPercent)+"%")
	}
	if n.Rate != "" {
		args = append(args, "rate", n.Rate)
	}
	return args, nil
}

type PairSpec struct {
	ServerNS       string
	ClientNS       string
	ServerVeth     string
	ClientVeth     string
	ServerAddrCIDR string
	ClientAddrCIDR string
	ServerEgress   Netem
	ClientEgress   Netem
}

func DefaultPair(prefix string) PairSpec {
	if prefix == "" {
		prefix = "rudpbench"
	}
	return PairSpec{
		ServerNS:       prefix + "-srv",
		ClientNS:       prefix + "-cli",
		ServerVeth:     prefix + "-vs",
		ClientVeth:     prefix + "-vc",
		ServerAddrCIDR: "10.200.0.1/24",
		ClientAddrCIDR: "10.200.0.2/24",
	}
}

func BuildSetupCommands(spec PairSpec) ([]Command, error) {
	if err := validatePair(spec); err != nil {
		return nil, err
	}
	cmds := []Command{
		{"ip", []string{"netns", "add", spec.ServerNS}},
		{"ip", []string{"netns", "add", spec.ClientNS}},
		{"ip", []string{"link", "add", spec.ServerVeth, "type", "veth", "peer", "name", spec.ClientVeth}},
		{"ip", []string{"link", "set", spec.ServerVeth, "netns", spec.ServerNS}},
		{"ip", []string{"link", "set", spec.ClientVeth, "netns", spec.ClientNS}},
		{"ip", []string{"-n", spec.ServerNS, "addr", "add", spec.ServerAddrCIDR, "dev", spec.ServerVeth}},
		{"ip", []string{"-n", spec.ClientNS, "addr", "add", spec.ClientAddrCIDR, "dev", spec.ClientVeth}},
		{"ip", []string{"-n", spec.ServerNS, "link", "set", "lo", "up"}},
		{"ip", []string{"-n", spec.ClientNS, "link", "set", "lo", "up"}},
		{"ip", []string{"-n", spec.ServerNS, "link", "set", spec.ServerVeth, "up"}},
		{"ip", []string{"-n", spec.ClientNS, "link", "set", spec.ClientVeth, "up"}},
	}
	if spec.ServerEgress.Enabled() {
		args, err := spec.ServerEgress.Args()
		if err != nil {
			return nil, err
		}
		cmds = append(cmds, Command{"tc", append([]string{"-n", spec.ServerNS, "qdisc", "replace", "dev", spec.ServerVeth, "root", "netem"}, args...)})
	}
	if spec.ClientEgress.Enabled() {
		args, err := spec.ClientEgress.Args()
		if err != nil {
			return nil, err
		}
		cmds = append(cmds, Command{"tc", append([]string{"-n", spec.ClientNS, "qdisc", "replace", "dev", spec.ClientVeth, "root", "netem"}, args...)})
	}
	return cmds, nil
}

func BuildQdiscShowCommands(spec PairSpec) ([]Command, error) {
	if err := validatePair(spec); err != nil {
		return nil, err
	}
	return []Command{
		{"tc", []string{"-n", spec.ServerNS, "-s", "qdisc", "show", "dev", spec.ServerVeth}},
		{"tc", []string{"-n", spec.ClientNS, "-s", "qdisc", "show", "dev", spec.ClientVeth}},
	}, nil
}

func BuildTeardownCommands(spec PairSpec) ([]Command, error) {
	if err := validatePair(spec); err != nil {
		return nil, err
	}
	return []Command{
		{"ip", []string{"netns", "del", spec.ServerNS}},
		{"ip", []string{"netns", "del", spec.ClientNS}},
	}, nil
}

type RunOptions struct {
	DryRun bool
	Stdout io.Writer
	Stderr io.Writer
}

func RunCommands(ctx context.Context, cmds []Command, opts RunOptions) error {
	stdout := opts.Stdout
	if stdout == nil {
		stdout = io.Discard
	}
	stderr := opts.Stderr
	if stderr == nil {
		stderr = io.Discard
	}
	for _, c := range cmds {
		if opts.DryRun {
			fmt.Fprintln(stdout, c.String())
			continue
		}
		cmd := exec.CommandContext(ctx, c.Name, c.Args...)
		cmd.Stdout = stdout
		cmd.Stderr = stderr
		if err := cmd.Run(); err != nil {
			return fmt.Errorf("%s: %w", c.String(), err)
		}
	}
	return nil
}

func CommandsString(cmds []Command) string {
	var b bytes.Buffer
	for _, c := range cmds {
		b.WriteString(c.String())
		b.WriteByte('\n')
	}
	return b.String()
}

func validatePair(spec PairSpec) error {
	for name, value := range map[string]string{
		"server_ns":        spec.ServerNS,
		"client_ns":        spec.ClientNS,
		"server_veth":      spec.ServerVeth,
		"client_veth":      spec.ClientVeth,
		"server_addr_cidr": spec.ServerAddrCIDR,
		"client_addr_cidr": spec.ClientAddrCIDR,
	} {
		if value == "" {
			return fmt.Errorf("%s is required", name)
		}
	}
	return nil
}

func formatPercent(v float64) string {
	return strconv.FormatFloat(v, 'f', -1, 64)
}
