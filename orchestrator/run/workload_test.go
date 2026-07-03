package run

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/netops"
)

func TestLookupWorkloadPlaneCells(t *testing.T) {
	// anchor セル(docs/profiles.md)がそのまま引けること
	w, ok := LookupWorkload("r20p128")
	if !ok {
		t.Fatal("r20p128 not found")
	}
	if w.RateLT != 20 || w.PayloadLT != 128 || w.RateMD != 1 || w.PayloadMD != 128 {
		t.Fatalf("r20p128 = %+v", w)
	}
	if !w.BroadcastLT || !w.BroadcastMD {
		t.Fatalf("plane cells must be broadcast: %+v", w)
	}

	w, ok = LookupWorkload("r60p200")
	if !ok || w.RateLT != 60 || w.PayloadLT != 200 {
		t.Fatalf("r60p200 = %+v ok=%v", w, ok)
	}

	if names := WorkloadNames(); len(names) != 11 {
		t.Fatalf("expected 9 plane cells + 2 synthetic, got %d: %v", len(names), names)
	}
	for _, name := range WorkloadNames() {
		if _, ok := LookupWorkload(name); !ok {
			t.Fatalf("WorkloadNames lists %q but lookup fails", name)
		}
	}
	if _, ok := LookupWorkload("r30p128"); ok {
		t.Fatal("r30p128 must not exist")
	}
}

func TestLookupWorkloadSynthetic(t *testing.T) {
	w, ok := LookupWorkload("echo")
	if !ok || w.RateLT != 50 || w.RateMD != 50 || w.PayloadLT != 64 || w.PayloadMD != 64 {
		t.Fatalf("echo = %+v ok=%v", w, ok)
	}
	if w.BroadcastLT || w.BroadcastMD {
		t.Fatalf("echo must not broadcast: %+v", w)
	}
	w, ok = LookupWorkload("reliable_echo")
	if !ok || w.RateLT != 0 || w.RateMD != 50 || w.PayloadMD != 64 {
		t.Fatalf("reliable_echo = %+v ok=%v", w, ok)
	}
}

func TestWorkloadClientArgs(t *testing.T) {
	w, _ := LookupWorkload("r20p1000")
	got := strings.Join(w.ClientArgs(), " ")
	want := "--rate-lt 20 --rate-md 1 --payload-lt 1000 --payload-md 128 --broadcast-lt --broadcast-md"
	if got != want {
		t.Fatalf("got %q want %q", got, want)
	}

	w, _ = LookupWorkload("reliable_echo")
	got = strings.Join(w.ClientArgs(), " ")
	want = "--rate-lt 0 --rate-md 50 --payload-md 64"
	if got != want {
		t.Fatalf("got %q want %q", got, want)
	}
}

func TestAutoDurationLossRule(t *testing.T) {
	w, _ := LookupWorkload("r60p200")
	// 無負荷極限 anchor(c4)× loss 最悪点(3%×b16、両方向):
	// uplink 4×61=244pps が律速 → 30×16/(0.03×244) ≈ 65.6s
	netem := &NetemRegime{
		ClientEgress: netops.Netem{DelayMS: 25, LossPercent: 3, LossBurstLen: 16},
		ServerEgress: netops.Netem{DelayMS: 25, LossPercent: 3, LossBurstLen: 16},
	}
	d := autoDuration(w, 4, netem)
	if d < 65*time.Second || d > 66*time.Second {
		t.Fatalf("expected ~65.6s, got %v", d)
	}

	// 高 pps では下限 10s に張り付く
	d = autoDuration(w, 100, netem)
	if d != 10*time.Second {
		t.Fatalf("expected 10s floor at high pps, got %v", d)
	}
}

func TestAutoDurationClamps(t *testing.T) {
	// 低 loss 率 × 低 pps は上限 120s に clamp(0.1%×b1, r10p128 c4 → 681s)
	w, _ := LookupWorkload("r10p128")
	netem := &NetemRegime{
		ClientEgress: netops.Netem{LossPercent: 0.1, LossBurstLen: 1},
	}
	if d := autoDuration(w, 4, netem); d != 120*time.Second {
		t.Fatalf("expected 120s cap, got %v", d)
	}

	// loss なし・netem なしは下限
	if d := autoDuration(w, 4, nil); d != 10*time.Second {
		t.Fatalf("expected 10s without netem, got %v", d)
	}
	if d := autoDuration(w, 4, &NetemRegime{ClientEgress: netops.Netem{DelayMS: 10}}); d != 10*time.Second {
		t.Fatalf("expected 10s without loss, got %v", d)
	}
}

func writeConfig(t *testing.T, body string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "config.json")
	if err := os.WriteFile(path, []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

const configCommon = `
 "transport": "enet",
 "server_command": ["srv"],
 "client_procs": 1,
 "total_conns": 4,
 "warmup": "1s",
 "drain": "1s",
 "output_dir": "out"
`

func TestLoadConfigWorkloadExpansion(t *testing.T) {
	path := writeConfig(t, `{
 "workload": "r20p128",
 "client_command": ["cli", "--host", "h"],
 "netem": {"client_egress": {"loss_pct": 3, "loss_burst_len": 16},
           "server_egress": {"loss_pct": 3, "loss_burst_len": 16}},`+configCommon+`}`)
	cfg, err := LoadConfig(path)
	if err != nil {
		t.Fatal(err)
	}
	args := strings.Join(cfg.ClientCommand.Args, " ")
	if !strings.Contains(args, "--rate-lt 20 --rate-md 1 --payload-lt 128 --payload-md 128 --broadcast-lt --broadcast-md") {
		t.Fatalf("workload args not appended: %q", args)
	}
	// c4 × r20p128: uplink 84pps → 30×16/(0.03×84) ≈ 190s → 120s cap
	if cfg.Duration.Duration != 120*time.Second {
		t.Fatalf("expected auto duration 120s, got %v", cfg.Duration.Duration)
	}
}

func TestLoadConfigWorkloadExplicitDurationWins(t *testing.T) {
	path := writeConfig(t, `{
 "workload": "echo",
 "duration": "3s",
 "client_command": ["cli"],`+configCommon+`}`)
	cfg, err := LoadConfig(path)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Duration.Duration != 3*time.Second {
		t.Fatalf("explicit duration must win, got %v", cfg.Duration.Duration)
	}
}

func TestLoadConfigWorkloadConflict(t *testing.T) {
	path := writeConfig(t, `{
 "workload": "echo",
 "client_command": ["cli", "--rate-lt", "30"],`+configCommon+`}`)
	if _, err := LoadConfig(path); err == nil || !strings.Contains(err.Error(), "conflicts") {
		t.Fatalf("expected conflict error, got %v", err)
	}
}

func TestLoadConfigUnknownWorkload(t *testing.T) {
	path := writeConfig(t, `{
 "workload": "r30p128",
 "client_command": ["cli"],`+configCommon+`}`)
	if _, err := LoadConfig(path); err == nil || !strings.Contains(err.Error(), "unknown workload") {
		t.Fatalf("expected unknown workload error, got %v", err)
	}
}

func TestPrepareClampsClientProcs(t *testing.T) {
	cfg := RunConfig{
		Transport:     "enet",
		ServerCommand: CommandConfig{Path: "srv"},
		ClientCommand: CommandConfig{Path: "cli"},
		ClientProcs:   4,
		TotalConns:    1,
		Duration:      Duration{time.Second},
		OutputDir:     "out",
	}
	got, err := cfg.Prepare()
	if err != nil {
		t.Fatal(err)
	}
	if got.ClientProcs != 1 {
		t.Fatalf("client_procs=%d, want clamp to 1", got.ClientProcs)
	}
}
