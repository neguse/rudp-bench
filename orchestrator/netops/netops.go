package netops

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"math"
	"os/exec"
	"strconv"
	"strings"

	"github.com/neguse/rudp-bench/orchestrator/netops/losstrace"
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
	// LossBurstLen > 0 のとき、単純ランダム loss ではなく Gilbert-Elliott
	// (netem gemodel)を使う。LossPercent = 平均 loss 率、LossBurstLen =
	// 平均バースト長(packets)。2状態モデル(bad 状態で必ず loss)で
	// r = 1/burst_len、p = loss·r/(1−loss) から遷移確率を逆算する。
	LossBurstLen float64 `json:"loss_burst_len,omitempty"`
	// LossSeed != 0 のとき loss を netem の乱数ではなく決定的 trace
	// (eBPF/TCX、netops/losstrace)で注入する。同一 seed なら全 run・全
	// transport に同じ落ち方が再生される(再生計画 D2)。delay/rate は
	// 従来どおり netem。LossBurstLen の意味は同じ(trace 生成側で使う)。
	LossSeed uint64 `json:"loss_seed,omitempty"`
	// TraceBits: trace 長(パケット数、2の冪)。0 なら losstrace.DefaultBits
	TraceBits int    `json:"trace_bits,omitempty"`
	Rate      string `json:"rate,omitempty"`
	Limit     int    `json:"limit,omitempty"`
}

// gemodelParams は (平均 loss 率, 平均バースト長) から Gilbert-Elliott の
// 遷移確率 p(good→bad), r(bad→good) をパーセントで返す。
func (n Netem) gemodelParams() (p, r float64, err error) {
	loss := n.LossPercent / 100.0
	if loss <= 0 || loss >= 1 {
		return 0, 0, fmt.Errorf("gemodel requires 0 < loss_pct < 100, got %g", n.LossPercent)
	}
	if n.LossBurstLen < 1 {
		return 0, 0, fmt.Errorf("loss_burst_len must be >= 1, got %g", n.LossBurstLen)
	}
	r = 1.0 / n.LossBurstLen
	p = loss * r / (1.0 - loss)
	if p >= 1 {
		return 0, 0, fmt.Errorf("loss_pct %g with burst_len %g is infeasible (p >= 1)", n.LossPercent, n.LossBurstLen)
	}
	return p * 100.0, r * 100.0, nil
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
	// LossSeed 指定時、loss は決定的 trace(eBPF)側が担うので netem には
	// 渡さない(delay/rate のみ)
	if n.LossPercent != 0 && n.LossSeed == 0 {
		if n.LossBurstLen > 0 {
			p, r, err := n.gemodelParams()
			if err != nil {
				return nil, err
			}
			// 1-h=100%(bad 状態で必ず loss)、1-k=0%(good 状態で loss なし)を明示。
			// tc に渡す桁は 1e-4 % で丸める(echo back 検証は相対 10% 許容)
			roundPct := func(v float64) string {
				return strconv.FormatFloat(math.Round(v*1e4)/1e4, 'f', -1, 64)
			}
			args = append(args, "loss", "gemodel",
				roundPct(p)+"%", roundPct(r)+"%", "100%", "0%")
		} else {
			args = append(args, "loss", formatPercent(n.LossPercent)+"%")
		}
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
	LinkMTUBytes   int `json:"link_mtu_bytes"`
	ServerEgress   Netem
	ClientEgress   Netem
	// DisableOffloads disables packet segmentation/coalescing features on both
	// veth endpoints. This is required when qdisc packet counts are interpreted
	// as application-message exposure evidence.
	DisableOffloads bool `json:"disable_offloads,omitempty"`
	// SelfExe: orchestrator 自身の実行ファイル path。LossSeed(決定的 loss
	// 注入)を使う egress がある場合に必須(`ip netns exec <ns> <SelfExe>
	// losstrace attach ...` を setup commands に組み込むため)
	SelfExe string
}

// lossTraceBpffs は決定的 loss 注入用の専用 bpffs マウント。
// /sys/fs/bpf を使わないのは、`ip netns exec` が /sys を張り替えるため
// netns 内から見えないから。/run 配下のマウントは exec のマウント名前空間に
// そのまま引き継がれ、pin(共有 superblock 上の dentry)はホスト側に残る。
const lossTraceBpffs = "/run/rudpbench-bpf"

// lossTracePinDir は netns ごとの TCX link pin 置き場。
func lossTracePinDir(ns string) string {
	return lossTraceBpffs + "/" + ns
}

// lossTraceMountCommand は専用 bpffs の冪等マウント。
func lossTraceMountCommand() Command {
	return Command{"sh", []string{"-c",
		"mountpoint -q " + lossTraceBpffs + " || { mkdir -p " + lossTraceBpffs +
			" && mount -t bpf bpf " + lossTraceBpffs + "; }"}}
}

// LossTraceReset は決定的 loss 注入のパケットカウンタを 0 に戻す。
// netem gate(ping/iperf3)等が消費した trace 位置をリセットし、計測本体が
// 毎 run 同じ位置から trace を再生するようにする。pin はホスト側 bpffs に
// あるため netns に入る必要はない。trace 未使用の egress は何もしない。
func LossTraceReset(spec PairSpec) error {
	if spec.ServerEgress.LossSeed != 0 && spec.ServerEgress.LossPercent > 0 {
		if err := losstrace.ResetCounter(spec.ServerVeth, lossTracePinDir(spec.ServerNS)); err != nil {
			return fmt.Errorf("reset server loss trace: %w", err)
		}
	}
	if spec.ClientEgress.LossSeed != 0 && spec.ClientEgress.LossPercent > 0 {
		if err := losstrace.ResetCounter(spec.ClientVeth, lossTracePinDir(spec.ClientNS)); err != nil {
			return fmt.Errorf("reset client loss trace: %w", err)
		}
	}
	return nil
}

// LossTraceEnabled は決定的 loss 注入を使う egress があるかを返す。
func LossTraceEnabled(spec PairSpec) bool {
	return (spec.ServerEgress.LossSeed != 0 && spec.ServerEgress.LossPercent > 0) ||
		(spec.ClientEgress.LossSeed != 0 && spec.ClientEgress.LossPercent > 0)
}

// lossTraceCommands は egress に決定的 loss 注入が指定されていれば
// attach コマンドを返す。
func lossTraceCommands(spec PairSpec, ns, veth string, egress Netem) ([]Command, error) {
	if egress.LossSeed == 0 || egress.LossPercent <= 0 {
		return nil, nil
	}
	if spec.SelfExe == "" {
		return nil, fmt.Errorf("loss_seed requires PairSpec.SelfExe (orchestrator binary path)")
	}
	args := []string{"netns", "exec", ns, spec.SelfExe, "losstrace", "attach",
		"-dev", veth,
		"-seed", strconv.FormatUint(egress.LossSeed, 10),
		"-loss-pct", formatPercent(egress.LossPercent),
		"-pin-dir", lossTracePinDir(ns),
	}
	if egress.LossBurstLen > 0 {
		args = append(args, "-burst", strconv.FormatFloat(egress.LossBurstLen, 'f', -1, 64))
	}
	if egress.TraceBits > 0 {
		args = append(args, "-bits", strconv.Itoa(egress.TraceBits))
	}
	return []Command{{"ip", args}}, nil
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
		LinkMTUBytes:   1500,
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
		{"ip", []string{"-n", spec.ServerNS, "link", "set", "dev", spec.ServerVeth, "mtu", strconv.Itoa(spec.LinkMTUBytes)}},
		{"ip", []string{"-n", spec.ClientNS, "link", "set", "dev", spec.ClientVeth, "mtu", strconv.Itoa(spec.LinkMTUBytes)}},
		{"ip", []string{"-n", spec.ServerNS, "addr", "add", spec.ServerAddrCIDR, "dev", spec.ServerVeth}},
		{"ip", []string{"-n", spec.ClientNS, "addr", "add", spec.ClientAddrCIDR, "dev", spec.ClientVeth}},
		{"ip", []string{"-n", spec.ServerNS, "link", "set", "lo", "up"}},
		{"ip", []string{"-n", spec.ClientNS, "link", "set", "lo", "up"}},
		{"ip", []string{"-n", spec.ServerNS, "link", "set", spec.ServerVeth, "up"}},
		{"ip", []string{"-n", spec.ClientNS, "link", "set", spec.ClientVeth, "up"}},
	}
	if spec.DisableOffloads {
		cmds = append(cmds,
			buildDisableOffloadsCommand(spec.ServerNS, spec.ServerVeth),
			buildDisableOffloadsCommand(spec.ClientNS, spec.ClientVeth),
		)
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
	// 決定的 loss 注入(loss_seed 指定時)。qdisc 設定後に attach する
	srvTrace, err := lossTraceCommands(spec, spec.ServerNS, spec.ServerVeth, spec.ServerEgress)
	if err != nil {
		return nil, err
	}
	cliTrace, err := lossTraceCommands(spec, spec.ClientNS, spec.ClientVeth, spec.ClientEgress)
	if err != nil {
		return nil, err
	}
	if len(srvTrace) > 0 || len(cliTrace) > 0 {
		cmds = append(cmds, lossTraceMountCommand())
	}
	cmds = append(cmds, srvTrace...)
	cmds = append(cmds, cliTrace...)
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
	cmds := []Command{
		{"ip", []string{"netns", "del", spec.ServerNS}},
		{"ip", []string{"netns", "del", spec.ClientNS}},
	}
	// 決定的 loss 注入の pin 掃除。netns 削除で TCX link 自体は外れるが、
	// bpffs 上の pin ファイルは残るため消す(冪等)
	if spec.ServerEgress.LossSeed != 0 {
		cmds = append(cmds, Command{"rm", []string{"-rf", lossTracePinDir(spec.ServerNS)}})
	}
	if spec.ClientEgress.LossSeed != 0 {
		cmds = append(cmds, Command{"rm", []string{"-rf", lossTracePinDir(spec.ClientNS)}})
	}
	return cmds, nil
}

type RunOptions struct {
	DryRun bool
	Stdout io.Writer
	Stderr io.Writer
}

func RunCommands(ctx context.Context, cmds []Command, opts RunOptions) error {
	for _, command := range cmds {
		if err := runCommand(ctx, command, opts); err != nil {
			return err
		}
	}
	return nil
}

// RunCommandsBestEffort executes every command and joins failures. Teardown
// paths use it so one missing namespace cannot leave another namespace behind.
func RunCommandsBestEffort(ctx context.Context, cmds []Command, opts RunOptions) error {
	var errs []error
	for _, command := range cmds {
		if err := runCommand(ctx, command, opts); err != nil {
			errs = append(errs, err)
		}
	}
	return errors.Join(errs...)
}

func runCommand(ctx context.Context, command Command, opts RunOptions) error {
	stdout := opts.Stdout
	if stdout == nil {
		stdout = io.Discard
	}
	stderr := opts.Stderr
	if stderr == nil {
		stderr = io.Discard
	}
	if opts.DryRun {
		fmt.Fprintln(stdout, command.String())
		return nil
	}
	cmd := exec.CommandContext(ctx, command.Name, command.Args...)
	cmd.Stdout = stdout
	cmd.Stderr = stderr
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("%s: %w", command.String(), err)
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
	if spec.LinkMTUBytes <= 0 || spec.LinkMTUBytes > 65535 {
		return fmt.Errorf("link_mtu_bytes must be between 1 and 65535")
	}
	return nil
}

func formatPercent(v float64) string {
	return strconv.FormatFloat(v, 'f', -1, 64)
}
