package run

import (
	"fmt"
	"strconv"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/netops"
)

// Workload は docs/profiles.md の負荷平面のセル(または synthetic baseline)。
// 平面セルは lt が可変 2 軸、md は 1Hz×128B broadcast 固定。
type Workload struct {
	Name        string
	RateLT      float64 // pps(0 = stream なし)
	RateMD      float64
	PayloadLT   int // bytes
	PayloadMD   int
	BroadcastLT bool
	BroadcastMD bool
}

var planeRates = []int{10, 20, 60}
var planePayloads = []int{128, 200, 1000}

// LookupWorkload はセル名(r{pps}p{bytes} / echo / reliable_echo)を解決する。
func LookupWorkload(name string) (Workload, bool) {
	switch name {
	case "echo":
		return Workload{Name: name, RateLT: 50, RateMD: 50, PayloadLT: 64, PayloadMD: 64}, true
	case "reliable_echo":
		return Workload{Name: name, RateMD: 50, PayloadMD: 64}, true
	}
	for _, r := range planeRates {
		for _, p := range planePayloads {
			if name == planeCellName(r, p) {
				return Workload{
					Name:        name,
					RateLT:      float64(r),
					PayloadLT:   p,
					RateMD:      1,
					PayloadMD:   128,
					BroadcastLT: true,
					BroadcastMD: true,
				}, true
			}
		}
	}
	return Workload{}, false
}

func planeCellName(rate, payload int) string {
	return fmt.Sprintf("r%dp%d", rate, payload)
}

// WorkloadNames は定義済み workload 名の一覧(エラーメッセージ用)。
func WorkloadNames() []string {
	names := make([]string, 0, len(planeRates)*len(planePayloads)+2)
	for _, r := range planeRates {
		for _, p := range planePayloads {
			names = append(names, planeCellName(r, p))
		}
	}
	return append(names, "echo", "reliable_echo")
}

// ClientArgs は全 client 実装共通のフラグ面に展開する。
func (w Workload) ClientArgs() []string {
	args := []string{
		"--rate-lt", formatRate(w.RateLT),
		"--rate-md", formatRate(w.RateMD),
	}
	if w.RateLT > 0 {
		args = append(args, "--payload-lt", strconv.Itoa(w.PayloadLT))
	}
	if w.RateMD > 0 {
		args = append(args, "--payload-md", strconv.Itoa(w.PayloadMD))
	}
	if w.BroadcastLT {
		args = append(args, "--broadcast-lt")
	}
	if w.BroadcastMD {
		args = append(args, "--broadcast-md")
	}
	return args
}

func formatRate(r float64) string {
	return strconv.FormatFloat(r, 'f', -1, 64)
}

// perConnRate は 1 conn あたりの送信 pps。
func (w Workload) perConnRate() float64 {
	return w.RateLT + w.RateMD
}

// linkPPS は netem が掛かるリンクの集約 pps(方向別)。
// uplink(client egress)は全 conn の送信合算。downlink(server egress)は
// echo で同数、broadcast で conn 数倍の fanout。
func (w Workload) linkPPS(totalConns int) (uplink, downlink float64) {
	n := float64(totalConns)
	uplink = n * w.perConnRate()
	downlink = 0
	for _, s := range []struct {
		rate      float64
		broadcast bool
	}{{w.RateLT, w.BroadcastLT}, {w.RateMD, w.BroadcastMD}} {
		out := n * s.rate
		if s.broadcast {
			out *= n
		}
		downlink += out
	}
	return uplink, downlink
}

const (
	// lossEventTarget は loss セルの duration 規則(design spec)の期待イベント数。
	// 暫定値 — E2 の反復ばらつきで感度確認する。
	lossEventTarget = 30
	autoDurationMin = 10 * time.Second
	autoDurationMax = 120 * time.Second
)

// autoDuration は duration 未指定時の導出規則:
// duration ≥ lossEventTarget × burst長 / (loss率 × リンク集約 pps) を
// loss の掛かる方向ごとに評価して max を取り、[10s, 120s] に clamp する。
// loss がなければ下限を返す。
func autoDuration(w Workload, totalConns int, netem *NetemRegime) time.Duration {
	need := autoDurationMin
	if netem != nil {
		uplink, downlink := w.linkPPS(totalConns)
		for _, dir := range []struct {
			egress netops.Netem
			pps    float64
		}{{netem.ClientEgress, uplink}, {netem.ServerEgress, downlink}} {
			d := lossEventDuration(dir.egress.LossPercent, dir.egress.LossBurstLen, dir.pps)
			if d > need {
				need = d
			}
		}
	}
	if need > autoDurationMax {
		return autoDurationMax
	}
	return need
}

func lossEventDuration(lossPercent, burstLen, pps float64) time.Duration {
	if lossPercent <= 0 || pps <= 0 {
		return 0
	}
	if burstLen < 1 {
		burstLen = 1
	}
	seconds := lossEventTarget * burstLen / (lossPercent / 100.0 * pps)
	return time.Duration(seconds * float64(time.Second))
}
