package run

import (
	"strings"
	"testing"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/netops"
)

func scheduleFixture() RunConfig {
	return RunConfig{
		Transport:     "t",
		ServerCommand: CommandConfig{Path: "s"},
		ClientCommand: CommandConfig{Path: "c"},
		ClientProcs:   1, TotalConns: 1,
		Duration: Duration{30 * time.Second}, OutputDir: "out",
		Netem: &NetemRegime{
			ServerEgress: netops.Netem{DelayMS: 50},
			ClientEgress: netops.Netem{DelayMS: 50},
		},
		NetemSchedule: []NetemStep{
			{At: Duration{5 * time.Second}, ServerEgress: netops.Netem{DelayMS: 50, LossPercent: 100}, ClientEgress: netops.Netem{DelayMS: 50, LossPercent: 100}},
			{At: Duration{10 * time.Second}, ServerEgress: netops.Netem{DelayMS: 50}, ClientEgress: netops.Netem{DelayMS: 50}},
		},
	}
}

func TestNetemScheduleAcceptsOrderedSteps(t *testing.T) {
	cfg := scheduleFixture()
	if _, err := cfg.Prepare(); err != nil {
		t.Fatalf("valid schedule rejected: %v", err)
	}
}

func TestNetemScheduleRejectsInvalidConfigs(t *testing.T) {
	cases := []struct {
		name   string
		mutate func(*RunConfig)
		want   string
	}{
		{"no netem", func(c *RunConfig) { c.Netem = nil }, "netem_schedule requires netem"},
		{"steady warmup", func(c *RunConfig) { c.SteadyWarmup = true }, "netem_schedule requires a fixed warmup"},
		{"base loss", func(c *RunConfig) { c.Netem.ServerEgress.LossPercent = 1 }, "netem_schedule requires a loss-free base regime"},
		{"unordered", func(c *RunConfig) { c.NetemSchedule[1].At = Duration{time.Second} }, "netem_schedule must be ordered by at"},
		{"seeded step", func(c *RunConfig) { c.NetemSchedule[0].ClientEgress.LossSeed = 3 }, "netem_schedule[0].client_egress: loss_seed is not supported"},
		{"bad step", func(c *RunConfig) { c.NetemSchedule[0].ServerEgress.LossPercent = 101 }, "netem_schedule[0].server_egress"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			cfg := scheduleFixture()
			tc.mutate(&cfg)
			if _, err := cfg.Prepare(); err == nil || !strings.Contains(err.Error(), tc.want) {
				t.Fatalf("err=%v, want containing %q", err, tc.want)
			}
		})
	}
}

func TestNetemStepCommandsReplaceBothEgresses(t *testing.T) {
	pair := netops.DefaultPair("rbtest")
	step := NetemStep{ServerEgress: netops.Netem{DelayMS: 50, LossPercent: 100}, ClientEgress: netops.Netem{}}
	cmds, err := step.commands(pair)
	if err != nil {
		t.Fatal(err)
	}
	got := netops.CommandsString(cmds)
	want := "tc -n rbtest-srv qdisc replace dev rbtest-vs root netem limit 10000 delay 50ms loss 100%\n" +
		"tc -n rbtest-cli qdisc replace dev rbtest-vc root netem limit 10000\n"
	if got != want {
		t.Fatalf("commands mismatch\ngot:\n%s\nwant:\n%s", got, want)
	}
}
