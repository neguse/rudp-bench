package run

import (
	"context"
	"fmt"
	"os/exec"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/control"
	"github.com/neguse/rudp-bench/orchestrator/monotonic"
	"github.com/neguse/rudp-bench/orchestrator/netops"
)

// NetemStep は計測窓の開始時刻から At だけ経った時点で、両 egress の netem を差し替える。
// At は負でもよい(warmup 中の変更)。
type NetemStep struct {
	At           Duration     `json:"at"`
	ServerEgress netops.Netem `json:"server_egress"`
	ClientEgress netops.Netem `json:"client_egress"`
}

// AppliedNetemStep は差し替えの実施記録。時刻は計測窓と同じ CLOCK_MONOTONIC。
type AppliedNetemStep struct {
	Step      NetemStep        `json:"step"`
	PlannedNS int64            `json:"planned_ns"`
	AppliedNS int64            `json:"applied_ns"`
	Commands  []netops.Command `json:"commands"`
	Error     string           `json:"error,omitempty"`
}

func (s NetemStep) commands(pair netops.PairSpec) ([]netops.Command, error) {
	serverArgs, err := s.ServerEgress.Args()
	if err != nil {
		return nil, fmt.Errorf("server_egress: %w", err)
	}
	clientArgs, err := s.ClientEgress.Args()
	if err != nil {
		return nil, fmt.Errorf("client_egress: %w", err)
	}
	return []netops.Command{
		{Name: "tc", Args: append([]string{"-n", pair.ServerNS, "qdisc", "replace", "dev", pair.ServerVeth, "root", "netem"}, serverArgs...)},
		{Name: "tc", Args: append([]string{"-n", pair.ClientNS, "qdisc", "replace", "dev", pair.ClientVeth, "root", "netem"}, clientArgs...)},
	}, nil
}

func (cfg RunConfig) validateNetemSchedule() []error {
	if len(cfg.NetemSchedule) == 0 {
		return nil
	}
	var errs []error
	if cfg.Netem == nil {
		errs = append(errs, fmt.Errorf("netem_schedule requires netem"))
	}
	// 定常判定つき warmup では窓の開始が後から決まり、warmup 中の step 時刻を事前に置けない。
	if cfg.SteadyWarmup {
		errs = append(errs, fmt.Errorf("netem_schedule requires a fixed warmup (steady_warmup=false)"))
	}
	// loss evidence gate は窓の間 loss 率が一定である前提で実現値と設定値を照合するため、併用できない。
	if configuredLoss(cfg.Netem) {
		errs = append(errs, fmt.Errorf("netem_schedule requires a loss-free base regime"))
	}
	for i, step := range cfg.NetemSchedule {
		if i > 0 && step.At.Duration < cfg.NetemSchedule[i-1].At.Duration {
			errs = append(errs, fmt.Errorf("netem_schedule must be ordered by at"))
		}
		for name, egress := range map[string]netops.Netem{"server_egress": step.ServerEgress, "client_egress": step.ClientEgress} {
			if egress.LossSeed != 0 {
				errs = append(errs, fmt.Errorf("netem_schedule[%d].%s: loss_seed is not supported", i, name))
			}
		}
		if _, err := step.commands(netops.DefaultPair("")); err != nil {
			errs = append(errs, fmt.Errorf("netem_schedule[%d].%w", i, err))
		}
	}
	return errs
}

// runNetemSchedule は計測窓の配布を待ち、各 step をその時刻に適用する。
func runNetemSchedule(ctx context.Context, windowCh <-chan control.ScheduleMessage, pair netops.PairSpec, steps []NetemStep) []AppliedNetemStep {
	var window control.ScheduleMessage
	select {
	case window = <-windowCh:
	case <-ctx.Done():
		return nil
	}
	applied := make([]AppliedNetemStep, 0, len(steps))
	for _, step := range steps {
		rec := AppliedNetemStep{Step: step, PlannedNS: window.StartAtNS + step.At.Nanoseconds()}
		cmds, err := step.commands(pair)
		if err != nil {
			rec.Error = err.Error()
			applied = append(applied, rec)
			continue
		}
		rec.Commands = cmds
		now, err := monotonic.NowNS()
		if err != nil {
			rec.Error = err.Error()
			applied = append(applied, rec)
			continue
		}
		if wait := time.Duration(rec.PlannedNS - now); wait > 0 {
			select {
			case <-time.After(wait):
			case <-ctx.Done():
				return applied
			}
		}
		for _, c := range cmds {
			if out, err := exec.CommandContext(ctx, c.Name, c.Args...).CombinedOutput(); err != nil {
				rec.Error = fmt.Sprintf("%s: %v: %s", c.String(), err, out)
				break
			}
		}
		rec.AppliedNS, _ = monotonic.NowNS()
		applied = append(applied, rec)
	}
	return applied
}
