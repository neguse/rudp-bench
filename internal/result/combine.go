package result

import (
	"fmt"
	"strconv"
)

// SumCols are aggregated by summation across N client CSVs.
var SumCols = []string{
	"delivered",
	"accepted",
	"delivered_r",
	"delivered_u",
	"accepted_r",
	"accepted_u",
	"client_attempted",
	"client_accepted",
	"client_missed_pacing",
	"conn_peak",
	"conn_disc_transport",
	"conn_disc_peer",
}

// MaxCols are aggregated by max (worst observed) across N client CSVs.
var MaxCols = []string{
	"rss_mb",
	"connect_ms",
	"close_ms",
	"cpu_pct_peak",
	"client_tick_gap_p99_us",
	"client_tick_gap_max_us",
	"client_pacing_lag_p99_us",
	"client_pacing_lag_max_us",
	"client_recv_drained_p99",
	"client_recv_drained_max",
	"client_outstanding_max",
}

// RTT percentile columns, recomputed from merged bins (fallback: max of procs).
var rttRCols = []string{"rtt_r_p50_us", "rtt_r_p95_us", "rtt_r_p99_us"}
var rttUCols = []string{"rtt_u_p50_us", "rtt_u_p95_us", "rtt_u_p99_us"}

// Columns passed through from the first client CSV.
var passthroughFromFirst = []string{
	"library",
	"encryption",
	"phase",
	"rate_r",
	"rate_u",
	"size",
	"loss",
	"duration_s",
	"mode",
	"idle_policy",
	"flush_policy",
	"delivery_dedup_policy",
}

// toInt parses a string to int, returning 0 on failure (matches Python to_int).
func toInt(s string) int {
	if s == "" {
		return 0
	}
	v, err := strconv.Atoi(s)
	if err == nil {
		return v
	}
	f, err := strconv.ParseFloat(s, 64)
	if err != nil {
		return 0
	}
	return int(f)
}

// toFloat parses a string to float64, returning 0.0 on failure (matches Python to_float).
func toFloat(s string) float64 {
	if s == "" {
		return 0.0
	}
	v, err := strconv.ParseFloat(s, 64)
	if err != nil {
		return 0.0
	}
	return v
}

// attemptedTarget computes the expected total attempted messages for a row.
func attemptedTarget(row map[string]string, connsTotal int) float64 {
	combinedRate := toInt(row["rate_r"]) + toInt(row["rate_u"])
	conns := toInt(row["conns"])
	durationS := toInt(row["duration_s"])
	if combinedRate > 0 && conns > 0 && durationS > 0 {
		expectedPerSendVal := 1
		if row["mode"] == "broadcast" {
			expectedPerSendVal = connsTotal
		}
		return float64(combinedRate * conns * durationS * expectedPerSendVal)
	}
	attempted := toInt(row["client_attempted"])
	attemptedRatio := toFloat(row["client_attempted_ratio"])
	if attemptedRatio > 0.0 {
		return float64(attempted) / attemptedRatio
	}
	return float64(attempted)
}

// combineThroughput aggregates throughput from individual rows.
func combineThroughput(rows []map[string]string) (mbps float64, msgPerSec int) {
	durationS := 0
	for _, r := range rows {
		d := toInt(r["duration_s"])
		if d > durationS {
			durationS = d
		}
	}
	if durationS == 0 {
		durationS = 1
	}
	var bytesSum int64
	var msgsSum int64
	for _, r := range rows {
		msgs := int64(toInt(r["msg_per_sec"])) * int64(durationS)
		size := int64(toInt(r["size"]))
		bytesSum += msgs * size
		msgsSum += msgs
	}
	mbps = float64(bytesSum) * 8.0 / (float64(durationS) * 1_000_000.0)
	msgPerSec = int(msgsSum / int64(durationS))
	return
}

// CombineClients combines N parallel client raw CSVs and their RTT bin
// sidecars into one client-shaped CSV.
func CombineClients(clientCSVs []string, binsR, binsU []string, out string, connsTotal int) error {
	if len(clientCSVs) == 0 {
		return fmt.Errorf("no client csvs provided")
	}

	rows := make([]map[string]string, 0, len(clientCSVs))
	for _, p := range clientCSVs {
		row, err := ReadCSVRow(p)
		if err != nil {
			return fmt.Errorf("reading %s: %w", p, err)
		}
		if row == nil {
			return fmt.Errorf("%s: empty CSV", p)
		}
		rows = append(rows, row)
	}
	first := rows[0]

	// Merge RTT histograms.
	histR := &Histogram{}
	for _, p := range binsR {
		h, err := ReadHistogram(p)
		if err != nil {
			return fmt.Errorf("reading histogram %s: %w", p, err)
		}
		histR.MergeFrom(h)
	}
	histU := &Histogram{}
	for _, p := range binsU {
		h, err := ReadHistogram(p)
		if err != nil {
			return fmt.Errorf("reading histogram %s: %w", p, err)
		}
		histU.MergeFrom(h)
	}

	combined := map[string]string{}
	for _, col := range passthroughFromFirst {
		combined[col] = first[col]
	}
	combined["conns"] = strconv.Itoa(connsTotal)

	// Sum columns.
	for _, col := range SumCols {
		sum := 0
		for _, r := range rows {
			sum += toInt(r[col])
		}
		combined[col] = strconv.Itoa(sum)
	}

	// Max columns.
	for _, col := range MaxCols {
		mx := toInt(rows[0][col])
		for _, r := range rows[1:] {
			v := toInt(r[col])
			if v > mx {
				mx = v
			}
		}
		combined[col] = strconv.Itoa(mx)
	}

	// RTT percentiles from merged bins, with fallback to max of procs.
	rttValue := func(hist *Histogram, p float64, col string) string {
		if hist.Count > 0 {
			return strconv.Itoa(hist.PercentileUS(p))
		}
		mx := toInt(rows[0][col])
		for _, r := range rows[1:] {
			v := toInt(r[col])
			if v > mx {
				mx = v
			}
		}
		return strconv.Itoa(mx)
	}
	percentiles := []float64{0.50, 0.95, 0.99}
	for i, col := range rttRCols {
		combined[col] = rttValue(histR, percentiles[i], col)
	}
	for i, col := range rttUCols {
		combined[col] = rttValue(histU, percentiles[i], col)
	}

	// Aggregate throughput.
	mbps, msgPerSec := combineThroughput(rows)
	combined["throughput_mbps"] = fmt.Sprintf("%.3f", mbps)
	combined["msg_per_sec"] = strconv.Itoa(msgPerSec)

	// CPU: sum across procs.
	cpuSum := 0.0
	for _, r := range rows {
		cpuSum += toFloat(r["cpu_pct"])
	}
	combined["cpu_pct"] = fmt.Sprintf("%.2f", cpuSum)

	// Ratios re-derived from summed totals.
	attempted := toInt(combined["client_attempted"])
	accepted := toInt(combined["client_accepted"])
	delivered := toInt(combined["delivered"])
	targetAttempted := 0.0
	for _, r := range rows {
		targetAttempted += attemptedTarget(r, connsTotal)
	}
	if targetAttempted > 0 {
		combined["client_attempted_ratio"] = fmt.Sprintf("%.4f", float64(attempted)/targetAttempted)
	} else {
		combined["client_attempted_ratio"] = "0.0000"
	}
	if attempted > 0 {
		combined["client_accepted_ratio"] = fmt.Sprintf("%.4f", float64(accepted)/float64(attempted))
	} else {
		combined["client_accepted_ratio"] = "0.0000"
	}
	if accepted > 0 {
		combined["delivery_ratio"] = fmt.Sprintf("%.4f", float64(delivered)/float64(accepted))
	} else {
		combined["delivery_ratio"] = "0.0000"
	}

	// tick_ok from the AGGREGATE ratios.
	aggAttempted := toFloat(combined["client_attempted_ratio"])
	aggAccepted := toFloat(combined["client_accepted_ratio"])
	if aggAttempted >= 0.99 && aggAccepted >= 0.99 {
		combined["client_tick_ok"] = "1"
	} else {
		combined["client_tick_ok"] = "0"
	}

	// Read header from first input CSV to determine output column order.
	header, err := readCSVHeader(clientCSVs[0])
	if err != nil {
		return fmt.Errorf("reading header from %s: %w", clientCSVs[0], err)
	}

	// Build the output row using the header column order.
	outRow := make(map[string]string, len(header))
	for _, col := range header {
		if v, ok := combined[col]; ok {
			outRow[col] = v
		} else {
			outRow[col] = first[col]
		}
	}

	return WriteCSV(out, header, []map[string]string{outRow})
}
