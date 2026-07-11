package netops

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"math"
	"os/exec"
	"regexp"
	"strconv"
	"strings"
	"time"
)

// netem 実効値の pre-run gate(校正スイート §3)。
// 設定値の echo back(tc の読み戻し)だけでは「設定はされているが実効が違う」
// 型のアーティファクト(v1 の netem limit=1000 等)を検出できないため、
// ping / iperf3 を netns 内で実際に流して実測が設定と一致することを確認する。
//
// 方向の対応: 各 netns の veth root qdisc は egress にかかるため、
// client→server のトラフィックは client_egress のみを、逆方向は
// server_egress のみを通る。iperf3 は通常モードで client→server、
// -R で server→client を測る。ping の RTT は両方向の delay 合計。

type NetemGateReport struct {
	PingAvgMS      float64  `json:"ping_avg_ms"`
	ExpectedRTTMS  float64  `json:"expected_rtt_ms"`
	LossC2SPct     float64  `json:"loss_c2s_pct"`
	LossS2CPct     float64  `json:"loss_s2c_pct"`
	ExpectedC2SPct float64  `json:"expected_c2s_pct"`
	ExpectedS2CPct float64  `json:"expected_s2c_pct"`
	Failures       []string `json:"failures,omitempty"`
}

func (r NetemGateReport) OK() bool { return len(r.Failures) == 0 }

// ping の "rtt min/avg/max/mdev = 50.1/51.2/60.3/2.2 ms" 行から avg を取る。
var pingRTTRE = regexp.MustCompile(`rtt [^=]*= *[0-9.]+/([0-9.]+)/`)

func ParsePingAvgMS(output string) (float64, error) {
	m := pingRTTRE.FindStringSubmatch(output)
	if m == nil {
		return 0, fmt.Errorf("parse ping output: rtt line not found")
	}
	return strconv.ParseFloat(m[1], 64)
}

// iperf3 --json の UDP 結果から loss%(end.sum.lost_percent)を取る。
func ParseIperfLossPct(jsonOut string) (float64, error) {
	var doc struct {
		End struct {
			Sum struct {
				LostPercent *float64 `json:"lost_percent"`
			} `json:"sum"`
		} `json:"end"`
		Error string `json:"error"`
	}
	if err := json.Unmarshal([]byte(jsonOut), &doc); err != nil {
		return 0, fmt.Errorf("parse iperf3 json: %w", err)
	}
	if doc.Error != "" {
		return 0, fmt.Errorf("iperf3: %s", doc.Error)
	}
	if doc.End.Sum.LostPercent == nil {
		return 0, fmt.Errorf("iperf3 json has no end.sum.lost_percent")
	}
	return *doc.End.Sum.LostPercent, nil
}

// 許容: RTT は ±(20% + 2ms)。
func checkRTT(actual, expected float64) error {
	tol := expected*0.2 + 2.0
	if diff := actual - expected; diff > tol || diff < -tol {
		return fmt.Errorf("ping avg %.2fms, want %.2fms ±%.2fms", actual, expected, tol)
	}
	return nil
}

// burst loss はイベント数が 1/burst 長になり実測分散が burst 長倍に膨らむため、
// 上側の相対許容を √burst でスケールする。下側は expected/4 を下限とし、
// 「netem が適用されていない(実測 ≈ 0)」の検出力は維持する。
func checkLoss(actual, expected, burstLen float64, direction string) error {
	if burstLen < 1 {
		burstLen = 1
	}
	upper := expected*(1+0.5*math.Sqrt(burstLen)) + 0.3
	lower := expected/4.0 - 0.3
	if actual > upper || actual < lower {
		return fmt.Errorf("iperf3 %s loss %.3f%%, want %.3f%% (allowed %.3f%%..%.3f%%, burst=%g)", direction, actual, expected, lower, upper, burstLen)
	}
	return nil
}

// RunNetemGate は netns ペア上で ping / iperf3 を流し、実測を設定と突き合わせる。
// 戻り値の error はツール実行自体の失敗。設定との不一致は Report.Failures に入る。
func RunNetemGate(ctx context.Context, spec PairSpec) (NetemGateReport, error) {
	report := NetemGateReport{
		ExpectedRTTMS:  float64(spec.ServerEgress.DelayMS + spec.ClientEgress.DelayMS),
		ExpectedC2SPct: spec.ClientEgress.LossPercent,
		ExpectedS2CPct: spec.ServerEgress.LossPercent,
	}
	serverAddr := strings.SplitN(spec.ServerAddrCIDR, "/", 2)[0]

	// RTT: client ns から server addr へ ping(20 発、0.1s 間隔 ≈ 2s)
	ping := exec.CommandContext(ctx, "ip", "netns", "exec", spec.ClientNS,
		"ping", "-c", "20", "-i", "0.1", "-q", serverAddr)
	out, err := ping.CombinedOutput()
	if err != nil {
		return report, fmt.Errorf("ping: %w (%s)", err, strings.TrimSpace(string(out)))
	}
	avg, err := ParsePingAvgMS(string(out))
	if err != nil {
		return report, err
	}
	report.PingAvgMS = avg
	if err := checkRTT(avg, report.ExpectedRTTMS); err != nil {
		report.Failures = append(report.Failures, err.Error())
	}

	// loss: iperf3 UDP。ベンチ相当のレートで十分なパケット数を流す
	// (2000 pkt 級: 1% なら期待 20 損失、σ≈4.4)。server は netns 内で -1(1接続で終了)
	srv := exec.CommandContext(ctx, "ip", "netns", "exec", spec.ServerNS,
		"iperf3", "-s", "-1", "-B", serverAddr, "--json")
	var srvOut bytes.Buffer
	srv.Stdout = &srvOut
	srv.Stderr = &srvOut
	if err := srv.Start(); err != nil {
		return report, fmt.Errorf("iperf3 server: %w", err)
	}
	srvReaped := false
	time.Sleep(500 * time.Millisecond) // listener 起動待ち(client にリトライがない)
	defer func() {
		if !srvReaped {
			_ = srv.Process.Kill()
			_ = srv.Wait()
		}
	}()

	runClient := func(reverse bool) (float64, error) {
		args := []string{"netns", "exec", spec.ClientNS,
			"iperf3", "-c", serverAddr, "-u", "-b", "1M", "-l", "128",
			"-t", "3", "--json"}
		if reverse {
			args = append(args, "-R")
		}
		// iperf3 は接続タイミングで一過性に失敗することがある(listener 準備
		// レース等)。tooling の flake で測定点を汚さないよう1回だけリトライ
		var lastErr error
		for attempt := 0; attempt < 2; attempt++ {
			if attempt > 0 {
				time.Sleep(time.Second)
			}
			cmd := exec.CommandContext(ctx, "ip", args...)
			out, err := cmd.CombinedOutput()
			if err != nil {
				lastErr = fmt.Errorf("iperf3 client(reverse=%v): %w (%s)", reverse, err, strings.TrimSpace(string(out)))
				continue
			}
			return ParseIperfLossPct(string(out))
		}
		return 0, lastErr
	}

	// c→s(client egress のみ通る)
	loss, err := runClient(false)
	if err != nil {
		return report, err
	}
	if err := srv.Wait(); err != nil {
		srvReaped = true
		return report, fmt.Errorf("iperf3 server: %w (%s)", err, strings.TrimSpace(srvOut.String()))
	}
	srvReaped = true
	report.LossC2SPct = loss
	// 決定的 loss 注入(loss_seed)では iperf3 のサンプル窓が固定 trace の
	// どこに当たるかで実測率が大きくぶれる(3%×burst16 はバースト間隔
	// ~533 パケットで、3s サンプルが 0% を観測するのは正常)ため、率の
	// サンプル検証は統計的に無効 — skip する。trace の正しさは losstrace の
	// 単体テスト(実現率・バースト長)と ping のビット単位一致で担保済み。
	// RTT 検証(netem delay)は従来どおり行う
	if spec.ClientEgress.LossSeed == 0 {
		if err := checkLoss(loss, report.ExpectedC2SPct, spec.ClientEgress.LossBurstLen, "c2s"); err != nil {
			report.Failures = append(report.Failures, err.Error())
		}
	}

	// s→c は別セッション(iperf3 -s -1 は1接続で終わるため再起動)
	srv2 := exec.CommandContext(ctx, "ip", "netns", "exec", spec.ServerNS,
		"iperf3", "-s", "-1", "-B", serverAddr, "--json")
	var srv2Out bytes.Buffer
	srv2.Stdout = &srv2Out
	srv2.Stderr = &srv2Out
	if err := srv2.Start(); err != nil {
		return report, fmt.Errorf("iperf3 server(2): %w", err)
	}
	srv2Reaped := false
	time.Sleep(500 * time.Millisecond)
	defer func() {
		if !srv2Reaped {
			_ = srv2.Process.Kill()
			_ = srv2.Wait()
		}
	}()
	loss, err = runClient(true)
	if err != nil {
		return report, err
	}
	if err := srv2.Wait(); err != nil {
		srv2Reaped = true
		return report, fmt.Errorf("iperf3 server(2): %w (%s)", err, strings.TrimSpace(srv2Out.String()))
	}
	srv2Reaped = true
	report.LossS2CPct = loss
	if spec.ServerEgress.LossSeed == 0 {
		if err := checkLoss(loss, report.ExpectedS2CPct, spec.ServerEgress.LossBurstLen, "s2c"); err != nil {
			report.Failures = append(report.Failures, err.Error())
		}
	}

	return report, nil
}
