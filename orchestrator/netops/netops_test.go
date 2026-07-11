package netops_test

import (
	"context"
	"strings"
	"testing"

	"github.com/neguse/rudp-bench/orchestrator/netops"
)

func TestBuildCommandsGolden(t *testing.T) {
	spec := netops.DefaultPair("rbtest")
	spec.ServerEgress = netops.Netem{DelayMS: 10, JitterMS: 2, LossPercent: 0.1, Rate: "100mbit", Limit: 1000}
	spec.ClientEgress = netops.Netem{DelayMS: 40, JitterMS: 5, LossPercent: 1.5, Rate: "50mbit", Limit: 2000}

	setup, err := netops.BuildSetupCommands(spec)
	if err != nil {
		t.Fatal(err)
	}
	show, err := netops.BuildQdiscShowCommands(spec)
	if err != nil {
		t.Fatal(err)
	}
	teardown, err := netops.BuildTeardownCommands(spec)
	if err != nil {
		t.Fatal(err)
	}
	got := netops.CommandsString(append(append(setup, show...), teardown...))
	want := strings.Join([]string{
		"ip netns add rbtest-srv",
		"ip netns add rbtest-cli",
		"ip link add rbtest-vs type veth peer name rbtest-vc",
		"ip link set rbtest-vs netns rbtest-srv",
		"ip link set rbtest-vc netns rbtest-cli",
		"ip -n rbtest-srv link set dev rbtest-vs mtu 1500",
		"ip -n rbtest-cli link set dev rbtest-vc mtu 1500",
		"ip -n rbtest-srv addr add 10.200.0.1/24 dev rbtest-vs",
		"ip -n rbtest-cli addr add 10.200.0.2/24 dev rbtest-vc",
		"ip -n rbtest-srv link set lo up",
		"ip -n rbtest-cli link set lo up",
		"ip -n rbtest-srv link set rbtest-vs up",
		"ip -n rbtest-cli link set rbtest-vc up",
		"tc -n rbtest-srv qdisc replace dev rbtest-vs root netem limit 1000 delay 10ms 2ms distribution normal loss 0.1% rate 100mbit",
		"tc -n rbtest-cli qdisc replace dev rbtest-vc root netem limit 2000 delay 40ms 5ms distribution normal loss 1.5% rate 50mbit",
		"tc -n rbtest-srv -s qdisc show dev rbtest-vs",
		"tc -n rbtest-cli -s qdisc show dev rbtest-vc",
		"ip netns del rbtest-srv",
		"ip netns del rbtest-cli",
		"",
	}, "\n")
	if got != want {
		t.Fatalf("commands mismatch\ngot:\n%s\nwant:\n%s", got, want)
	}
}

func TestBuildSetupCommandsDisablesPacketShapingOffloads(t *testing.T) {
	spec := netops.DefaultPair("rbtest")
	spec.DisableOffloads = true
	setup, err := netops.BuildSetupCommands(spec)
	if err != nil {
		t.Fatal(err)
	}
	got := netops.CommandsString(setup)
	for _, want := range []string{
		"ip netns exec rbtest-srv ethtool -K rbtest-vs tso off gso off gro off lro off tx-udp-segmentation off rx-udp-gro-forwarding off",
		"ip netns exec rbtest-cli ethtool -K rbtest-vc tso off gso off gro off lro off tx-udp-segmentation off rx-udp-gro-forwarding off",
	} {
		if !strings.Contains(got, want+"\n") {
			t.Fatalf("setup commands do not contain %q:\n%s", want, got)
		}
	}
}

func TestParseOffloadFeatures(t *testing.T) {
	raw := `Features for rbtest-vs:
tcp-segmentation-offload: off
generic-segmentation-offload: off
generic-receive-offload: off
large-receive-offload: off [fixed]
tx-udp-segmentation: off
rx-udp-gro-forwarding: off
`
	features, err := netops.ParseOffloadFeatures(raw)
	if err != nil {
		t.Fatal(err)
	}
	if got := features["large-receive-offload"]; got.Enabled || !got.Fixed {
		t.Fatalf("large-receive-offload = %+v", got)
	}
	if len(features) != 6 {
		t.Fatalf("features len = %d, want 6", len(features))
	}
	if _, err := netops.ParseOffloadFeatures("Features for rbtest-vs:\n"); err == nil {
		t.Fatal("empty feature output must fail")
	}
}

func TestValidateOffloadEvidenceRejectsEnabledMissingAndTamperedReadback(t *testing.T) {
	spec := netops.DefaultPair("rbtest")
	raw := `Features for rbtest-vs:
tcp-segmentation-offload: off
generic-segmentation-offload: off
generic-receive-offload: off
large-receive-offload: off
tx-udp-segmentation: off
rx-udp-gro-forwarding: off
`
	sample := netops.OffloadInterfaceEvidence{
		Namespace:    spec.ServerNS,
		Device:       spec.ServerVeth,
		LinkMTUBytes: spec.LinkMTUBytes,
		Raw:          raw,
		LinkRaw:      `[{"ifname":"rbtest-vs","mtu":1500}]`,
		Features:     map[string]netops.OffloadFeatureState{},
	}
	for _, feature := range netops.RequiredOffloadFeatures() {
		sample.Features[feature] = netops.OffloadFeatureState{}
	}
	evidence := netops.OffloadEvidence{
		Version:          netops.OffloadEvidenceVersion,
		RequiredFeatures: netops.RequiredOffloadFeatures(),
		Server:           sample,
		Client:           sample,
	}
	evidence.Client.Namespace = spec.ClientNS
	evidence.Client.Device = spec.ClientVeth
	evidence.Client.LinkRaw = `[{"ifname":"rbtest-vc","mtu":1500}]`
	evidence.SHA256 = netops.HashOffloadEvidence(evidence)
	if reasons := netops.ValidateOffloadEvidence(spec, &evidence); len(reasons) != 0 {
		t.Fatalf("valid evidence rejected: %v", reasons)
	}
	contradictory := evidence
	contradictory.Client.Raw = strings.Replace(contradictory.Client.Raw,
		"generic-receive-offload: off", "generic-receive-offload: on", 1)
	contradictory.SHA256 = netops.HashOffloadEvidence(contradictory)
	if reasons := netops.ValidateOffloadEvidence(spec, &contradictory); !strings.Contains(strings.Join(reasons, "\n"), "does not match raw readback") {
		t.Fatalf("contradictory raw/map evidence accepted: %v", reasons)
	}
	enabled := evidence.Client.Features["generic-receive-offload"]
	enabled.Enabled = true
	evidence.Client.Features["generic-receive-offload"] = enabled
	delete(evidence.Server.Features, "tx-udp-segmentation")
	reasons := netops.ValidateOffloadEvidence(spec, &evidence)
	joined := strings.Join(reasons, "\n")
	for _, want := range []string{"sha256=", "generic-receive-offload is on", "tx-udp-segmentation is unavailable"} {
		if !strings.Contains(joined, want) {
			t.Fatalf("reasons %v do not contain %q", reasons, want)
		}
	}
}

func TestParseLinkMTU(t *testing.T) {
	name, mtu, err := netops.ParseLinkMTU(`[{"ifname":"rbtest-vs","mtu":1500}]`)
	if err != nil || name != "rbtest-vs" || mtu != 1500 {
		t.Fatalf("link readback name=%q mtu=%d err=%v", name, mtu, err)
	}
	for _, raw := range []string{`[]`, `[{"ifname":"rbtest-vs","mtu":0}]`, `{}`} {
		if _, _, err := netops.ParseLinkMTU(raw); err == nil {
			t.Fatalf("invalid link readback accepted: %s", raw)
		}
	}
}

func TestRunCommandsBestEffortContinuesAfterFailure(t *testing.T) {
	var output strings.Builder
	err := netops.RunCommandsBestEffort(context.Background(), []netops.Command{
		{Name: "sh", Args: []string{"-c", "exit 7"}},
		{Name: "sh", Args: []string{"-c", "printf reached"}},
	}, netops.RunOptions{Stdout: &output})
	if err == nil || output.String() != "reached" {
		t.Fatalf("best-effort commands err=%v output=%q", err, output.String())
	}
}

func TestDryRunPrintsCommands(t *testing.T) {
	var out strings.Builder
	cmds := []netops.Command{{Name: "ip", Args: []string{"netns", "add", "x"}}}
	if err := netops.RunCommands(context.Background(), cmds, netops.RunOptions{DryRun: true, Stdout: &out}); err != nil {
		t.Fatal(err)
	}
	if got := out.String(); got != "ip netns add x\n" {
		t.Fatalf("dry-run output = %q", got)
	}
}

func TestParseQdiscShow(t *testing.T) {
	fixture := `
qdisc netem 8001: root refcnt 2 limit 1000 delay 10ms  2ms loss 0.1% rate 100mbit
 Sent 123456 bytes 789 pkt (dropped 3, overlimits 4 requeues 5)
 backlog 0b 0p requeues 5
`
	stats, err := netops.ParseQdiscShow(fixture)
	if err != nil {
		t.Fatal(err)
	}
	if len(stats) != 1 {
		t.Fatalf("stats len = %d", len(stats))
	}
	got := stats[0]
	if got.Kind != "netem" || !got.Root || got.Limit != 1000 || got.DelayMS != 10 || got.JitterMS != 2 || got.LossPercent != 0.1 || got.Rate != "100mbit" {
		t.Fatalf("parsed qdisc = %+v", got)
	}
	if got.SentBytes != 123456 || got.SentPackets != 789 || got.Dropped != 3 || got.Overlimits != 4 || got.Requeues != 5 {
		t.Fatalf("parsed counters = %+v", got)
	}
	if err := netops.ValidateNetemEcho(netops.Netem{DelayMS: 10, JitterMS: 2, LossPercent: 0.1, Rate: "100mbit", Limit: 1000}, got); err != nil {
		t.Fatal(err)
	}
}

func TestDeltaQdiscPairPreservesDirectionalCounters(t *testing.T) {
	serverBefore := netops.QdiscStats{Kind: "netem", SentBytes: 100, SentPackets: 10, Dropped: 1, Overlimits: 2, Requeues: 3}
	clientBefore := netops.QdiscStats{Kind: "netem", SentBytes: 200, SentPackets: 20, Dropped: 4, Overlimits: 5, Requeues: 6}
	serverAfter := netops.QdiscStats{Kind: "netem", SentBytes: 150, SentPackets: 17, Dropped: 3, Overlimits: 5, Requeues: 7}
	clientAfter := netops.QdiscStats{Kind: "netem", SentBytes: 280, SentPackets: 29, Dropped: 9, Overlimits: 11, Requeues: 13}
	before := netops.QdiscPairSnapshot{
		ServerEgress: netops.QdiscSample{Stats: &serverBefore},
		ClientEgress: netops.QdiscSample{Stats: &clientBefore},
	}
	after := netops.QdiscPairSnapshot{
		ServerEgress: netops.QdiscSample{Stats: &serverAfter},
		ClientEgress: netops.QdiscSample{Stats: &clientAfter},
	}
	delta := netops.DeltaQdiscPair(before, after)
	if len(delta.Errors) != 0 {
		t.Fatalf("delta errors = %v", delta.Errors)
	}
	if got := delta.ServerEgress; got == nil || got.SentBytes != 50 || got.SentPackets != 7 || got.Dropped != 2 || got.Overlimits != 3 || got.Requeues != 4 {
		t.Fatalf("server delta = %+v", got)
	}
	if got := delta.ClientEgress; got == nil || got.SentBytes != 80 || got.SentPackets != 9 || got.Dropped != 5 || got.Overlimits != 6 || got.Requeues != 7 {
		t.Fatalf("client delta = %+v", got)
	}
}

func TestValidateNetemEchoRejectsRandomLossCorrelation(t *testing.T) {
	stats, err := netops.ParseQdiscShow(`
qdisc netem 8001: root refcnt 2 limit 10000 loss 1% 25%
 Sent 1000 bytes 10 pkt (dropped 1, overlimits 0 requeues 0)
`)
	if err != nil || len(stats) != 1 {
		t.Fatalf("parse correlated qdisc stats=%v err=%v", stats, err)
	}
	if stats[0].LossCorrelationPercent != 25 {
		t.Fatalf("loss correlation=%g, want 25", stats[0].LossCorrelationPercent)
	}
	if err := netops.ValidateNetemEcho(netops.Netem{LossPercent: 1}, stats[0]); err == nil || !strings.Contains(err.Error(), "independent") {
		t.Fatalf("correlated random loss accepted: %v", err)
	}
}

func TestDeltaQdiscPairRejectsReadFailureAndCounterRegression(t *testing.T) {
	beforeStats := netops.QdiscStats{Kind: "netem", SentPackets: 10, Dropped: 2}
	afterStats := netops.QdiscStats{Kind: "netem", SentPackets: 9, Dropped: 3}
	delta := netops.DeltaQdiscPair(
		netops.QdiscPairSnapshot{
			ServerEgress: netops.QdiscSample{Error: "permission denied"},
			ClientEgress: netops.QdiscSample{Stats: &beforeStats},
		},
		netops.QdiscPairSnapshot{
			ServerEgress: netops.QdiscSample{Stats: &afterStats},
			ClientEgress: netops.QdiscSample{Stats: &afterStats},
		},
	)
	if delta.ServerEgress != nil || delta.ClientEgress != nil {
		t.Fatalf("invalid deltas must be absent: %+v", delta)
	}
	joined := strings.Join(delta.Errors, "\n")
	if !strings.Contains(joined, "permission denied") || !strings.Contains(joined, "counters regressed") {
		t.Fatalf("delta errors = %v", delta.Errors)
	}
}

func TestParseUDPStatsAndDelta(t *testing.T) {
	before := `
Ip: Forwarding DefaultTTL
Ip: 1 64
Udp: InDatagrams NoPorts InErrors OutDatagrams RcvbufErrors SndbufErrors InCsumErrors IgnoredMulti MemErrors
Udp: 100 2 3 200 4 0 0 0 0
`
	after := `
Udp: InDatagrams NoPorts InErrors OutDatagrams RcvbufErrors SndbufErrors InCsumErrors IgnoredMulti MemErrors
Udp: 110 2 9 210 7 0 0 0 0
`
	b, err := netops.ParseUDPStats(before)
	if err != nil {
		t.Fatal(err)
	}
	a, err := netops.ParseUDPStats(after)
	if err != nil {
		t.Fatal(err)
	}
	delta := netops.DeltaUDPStats(b, a)
	if delta.InErrors != 6 || delta.RcvbufErrors != 3 {
		t.Fatalf("delta = %+v", delta)
	}
}

func TestNetemGemodelArgs(t *testing.T) {
	n := netops.Netem{DelayMS: 25, JitterMS: 2, LossPercent: 1, LossBurstLen: 4}
	args, err := n.Args()
	if err != nil {
		t.Fatal(err)
	}
	got := strings.Join(args, " ")
	// loss 1% / burst 4 → r = 25%, p = 1·0.25/0.99 ≈ 0.2525%
	want := "limit 10000 delay 25ms 2ms distribution normal loss gemodel 0.2525% 25% 100% 0%"
	if got != want {
		t.Fatalf("args = %q, want %q", got, want)
	}
}

func TestParseGemodelEchoBack(t *testing.T) {
	out := `qdisc netem 8001: root refcnt 2 limit 10000 delay 25ms  2ms loss gemodel p 0.2525% r 25% 1-h 100% 1-k 0%
 Sent 1000 bytes 10 pkt (dropped 0, overlimits 0 requeues 0)
 backlog 0b 0p requeues 0
`
	stats, err := netops.ParseQdiscShow(out)
	if err != nil {
		t.Fatal(err)
	}
	if len(stats) != 1 {
		t.Fatalf("stats = %d, want 1", len(stats))
	}
	expected := netops.Netem{DelayMS: 25, JitterMS: 2, LossPercent: 1, LossBurstLen: 4, Limit: 10000}
	if err := netops.ValidateNetemEcho(expected, stats[0]); err != nil {
		t.Fatalf("validate: %v", err)
	}
}

func TestParsePingAvgMS(t *testing.T) {
	out := `20 packets transmitted, 20 received, 0% packet loss, time 1928ms
rtt min/avg/max/mdev = 50.062/51.244/60.310/2.216 ms`
	avg, err := netops.ParsePingAvgMS(out)
	if err != nil {
		t.Fatal(err)
	}
	if avg != 51.244 {
		t.Fatalf("avg = %g, want 51.244", avg)
	}
}

func TestParseIperfLossPct(t *testing.T) {
	ok := `{"end":{"sum":{"lost_percent":1.0416666666666667}}}`
	loss, err := netops.ParseIperfLossPct(ok)
	if err != nil {
		t.Fatal(err)
	}
	if loss < 1.04 || loss > 1.05 {
		t.Fatalf("loss = %g", loss)
	}
	if _, err := netops.ParseIperfLossPct(`{"error":"unable to connect"}`); err == nil {
		t.Fatal("want error for iperf3 error json")
	}
	if _, err := netops.ParseIperfLossPct(`{"end":{"sum":{}}}`); err == nil {
		t.Fatal("want error for missing lost_percent")
	}
	if _, err := netops.ParseIperfLossSample(`{"end":{"sum":{"lost_percent":1}}}`); err == nil {
		t.Fatal("want error for missing packets")
	}
}

func TestValidateNetemGateReportRejectsZeroObservedCanonicalLoss(t *testing.T) {
	spec := netops.PairSpec{ClientEgress: netops.Netem{LossPercent: 1}}
	report := &netops.NetemGateReport{
		ExpectedC2SPct: 1, LossC2SPct: 0, LossC2SPackets: 2000,
		LossS2CPackets: 2000,
	}
	reasons := netops.ValidateNetemGateReport(spec, report)
	if len(reasons) == 0 || !strings.Contains(strings.Join(reasons, "\n"), "allowed") {
		t.Fatalf("zero observed 1%% loss was accepted: %v", reasons)
	}
	report.LossC2SPct = 1
	if reasons := netops.ValidateNetemGateReport(spec, report); len(reasons) != 0 {
		t.Fatalf("valid gate report rejected: %v", reasons)
	}
	spec.ClientEgress.LossPercent = 0.01
	report.ExpectedC2SPct = 0.01
	report.LossC2SPct = 0
	if reasons := netops.ValidateNetemGateReport(spec, report); len(reasons) != 0 {
		t.Fatalf("underpowered tiny-loss gate should allow zero observations: %v", reasons)
	}
}
