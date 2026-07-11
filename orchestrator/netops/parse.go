package netops

import (
	"bufio"
	"fmt"
	"math"
	"regexp"
	"strconv"
	"strings"
)

type QdiscStats struct {
	Kind        string  `json:"kind"`
	Root        bool    `json:"root,omitempty"`
	Limit       int     `json:"limit,omitempty"`
	DelayMS     float64 `json:"delay_ms,omitempty"`
	JitterMS    float64 `json:"jitter_ms,omitempty"`
	LossPercent float64 `json:"loss_pct,omitempty"`
	// Non-zero only when tc reports correlated random loss. Conformance
	// requires independent loss, so this must remain zero there.
	LossCorrelationPercent float64 `json:"loss_correlation_pct,omitempty"`
	// gemodel 検出時のみ非ゼロ: p/(p+r) と 1/r から再構成した値
	LossBurstLen float64 `json:"loss_burst_len,omitempty"`
	Rate         string  `json:"rate,omitempty"`
	SentBytes    uint64  `json:"sent_bytes"`
	SentPackets  uint64  `json:"sent_packets"`
	Dropped      uint64  `json:"dropped"`
	Overlimits   uint64  `json:"overlimits"`
	Requeues     uint64  `json:"requeues"`
}

var sentLineRE = regexp.MustCompile(`Sent\s+(\d+)\s+bytes\s+(\d+)\s+pkt\s+\(dropped\s+(\d+),\s+overlimits\s+(\d+)\s+requeues\s+(\d+)\)`)

func ParseQdiscShow(output string) ([]QdiscStats, error) {
	var out []QdiscStats
	var current *QdiscStats
	scanner := bufio.NewScanner(strings.NewReader(output))
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" {
			continue
		}
		fields := strings.Fields(line)
		if len(fields) >= 2 && fields[0] == "qdisc" {
			stat := QdiscStats{Kind: fields[1]}
			parseQdiscFields(fields, &stat)
			out = append(out, stat)
			current = &out[len(out)-1]
			continue
		}
		if current != nil && strings.HasPrefix(line, "Sent ") {
			if err := parseSentLine(line, current); err != nil {
				return nil, err
			}
		}
	}
	if err := scanner.Err(); err != nil {
		return nil, err
	}
	if len(out) == 0 {
		return nil, fmt.Errorf("no qdisc records found")
	}
	return out, nil
}

func ValidateNetemEcho(expected Netem, actual QdiscStats) error {
	if !expected.Enabled() {
		if actual.Kind == "netem" {
			return fmt.Errorf("unexpected netem qdisc present")
		}
		return nil
	}
	if actual.Kind != "netem" {
		return fmt.Errorf("qdisc kind = %q, want netem", actual.Kind)
	}
	limit := expected.Limit
	if limit == 0 {
		limit = 10000
	}
	if actual.Limit != limit {
		return fmt.Errorf("netem limit = %d, want %d", actual.Limit, limit)
	}
	if !closeFloat(actual.DelayMS, float64(expected.DelayMS)) {
		return fmt.Errorf("netem delay_ms = %g, want %d", actual.DelayMS, expected.DelayMS)
	}
	if !closeFloat(actual.JitterMS, float64(expected.JitterMS)) {
		return fmt.Errorf("netem jitter_ms = %g, want %d", actual.JitterMS, expected.JitterMS)
	}
	if expected.LossBurstLen > 0 {
		// tc は百分率を丸めて表示するため、再構成値は相対誤差で比較する
		if !closeRelative(actual.LossPercent, expected.LossPercent, 0.1) {
			return fmt.Errorf("netem gemodel loss = %g%%, want %g%%", actual.LossPercent, expected.LossPercent)
		}
		if !closeRelative(actual.LossBurstLen, expected.LossBurstLen, 0.1) {
			return fmt.Errorf("netem gemodel burst_len = %g, want %g", actual.LossBurstLen, expected.LossBurstLen)
		}
	} else if !closeFloat(actual.LossPercent, expected.LossPercent) {
		return fmt.Errorf("netem loss_percent = %g, want %g", actual.LossPercent, expected.LossPercent)
	}
	if actual.LossCorrelationPercent != 0 {
		return fmt.Errorf("netem loss correlation = %g%%, want independent random loss (0%%)", actual.LossCorrelationPercent)
	}
	if expected.Rate != "" && !strings.EqualFold(actual.Rate, expected.Rate) {
		return fmt.Errorf("netem rate = %q, want %q", actual.Rate, expected.Rate)
	}
	return nil
}

func parseQdiscFields(fields []string, stat *QdiscStats) {
	for i := 0; i < len(fields); i++ {
		switch fields[i] {
		case "root":
			stat.Root = true
		case "limit":
			if i+1 < len(fields) {
				stat.Limit, _ = strconv.Atoi(fields[i+1])
			}
		case "delay":
			if i+1 < len(fields) {
				stat.DelayMS, _ = parseTCMS(fields[i+1])
			}
			if i+2 < len(fields) && looksLikeTCDuration(fields[i+2]) {
				stat.JitterMS, _ = parseTCMS(fields[i+2])
			}
		case "loss":
			if i+1 < len(fields) && fields[i+1] == "gemodel" {
				// 形式: loss gemodel p 0.25% r 25% 1-h 100% 1-k 0%
				var p, r float64
				for j := i + 2; j+1 < len(fields); j += 2 {
					label := fields[j]
					if label != "p" && label != "r" && label != "1-h" && label != "1-k" {
						break
					}
					v, err := strconv.ParseFloat(strings.TrimSuffix(fields[j+1], "%"), 64)
					if err != nil {
						break
					}
					switch label {
					case "p":
						p = v
					case "r":
						r = v
					}
				}
				if p+r > 0 {
					stat.LossPercent = p / (p + r) * 100.0
				}
				if r > 0 {
					stat.LossBurstLen = 100.0 / r
				}
			} else {
				var percentages []float64
				for j := i + 1; j < len(fields); j++ {
					if strings.HasSuffix(fields[j], "%") {
						value, err := strconv.ParseFloat(strings.TrimSuffix(fields[j], "%"), 64)
						if err == nil {
							percentages = append(percentages, value)
						}
					}
				}
				if len(percentages) > 0 {
					stat.LossPercent = percentages[0]
				}
				if len(percentages) > 1 {
					stat.LossCorrelationPercent = percentages[1]
				}
			}
		case "rate":
			if i+1 < len(fields) {
				stat.Rate = fields[i+1]
			}
		}
	}
}

func parseSentLine(line string, stat *QdiscStats) error {
	m := sentLineRE.FindStringSubmatch(line)
	if m == nil {
		return fmt.Errorf("parse tc sent line: %q", line)
	}
	values := make([]uint64, 0, 5)
	for _, part := range m[1:] {
		v, err := strconv.ParseUint(part, 10, 64)
		if err != nil {
			return err
		}
		values = append(values, v)
	}
	stat.SentBytes = values[0]
	stat.SentPackets = values[1]
	stat.Dropped = values[2]
	stat.Overlimits = values[3]
	stat.Requeues = values[4]
	return nil
}

func looksLikeTCDuration(s string) bool {
	return strings.HasSuffix(s, "us") || strings.HasSuffix(s, "ms") || strings.HasSuffix(s, "s")
}

func parseTCMS(s string) (float64, error) {
	switch {
	case strings.HasSuffix(s, "us"):
		v, err := strconv.ParseFloat(strings.TrimSuffix(s, "us"), 64)
		return v / 1000, err
	case strings.HasSuffix(s, "ms"):
		return strconv.ParseFloat(strings.TrimSuffix(s, "ms"), 64)
	case strings.HasSuffix(s, "s"):
		v, err := strconv.ParseFloat(strings.TrimSuffix(s, "s"), 64)
		return v * 1000, err
	default:
		return 0, fmt.Errorf("unknown tc duration unit %q", s)
	}
}

func closeFloat(a, b float64) bool {
	return math.Abs(a-b) < 0.000001
}

func closeRelative(a, b, tol float64) bool {
	if b == 0 {
		return math.Abs(a) < tol
	}
	return math.Abs(a-b)/math.Abs(b) <= tol
}

type UDPStats struct {
	InErrors     uint64
	RcvbufErrors uint64
}

func ParseUDPStats(snmp string) (UDPStats, error) {
	scanner := bufio.NewScanner(strings.NewReader(snmp))
	var headers []string
	for scanner.Scan() {
		fields := strings.Fields(scanner.Text())
		if len(fields) == 0 || fields[0] != "Udp:" {
			continue
		}
		if len(fields) < 2 {
			continue
		}
		if _, err := strconv.ParseUint(fields[1], 10, 64); err != nil {
			headers = fields[1:]
			continue
		}
		if headers == nil {
			return UDPStats{}, fmt.Errorf("Udp values without header")
		}
		values := fields[1:]
		if len(values) != len(headers) {
			return UDPStats{}, fmt.Errorf("Udp header/value length mismatch: %d != %d", len(headers), len(values))
		}
		byName := make(map[string]uint64, len(headers))
		for i, name := range headers {
			v, err := strconv.ParseUint(values[i], 10, 64)
			if err != nil {
				return UDPStats{}, fmt.Errorf("Udp %s: %w", name, err)
			}
			byName[name] = v
		}
		return UDPStats{
			InErrors:     byName["InErrors"],
			RcvbufErrors: byName["RcvbufErrors"],
		}, nil
	}
	if err := scanner.Err(); err != nil {
		return UDPStats{}, err
	}
	return UDPStats{}, fmt.Errorf("Udp section not found")
}

func DeltaUDPStats(before, after UDPStats) UDPStats {
	return UDPStats{
		InErrors:     saturatingDelta(before.InErrors, after.InErrors),
		RcvbufErrors: saturatingDelta(before.RcvbufErrors, after.RcvbufErrors),
	}
}

func saturatingDelta(before, after uint64) uint64 {
	if after < before {
		return 0
	}
	return after - before
}
