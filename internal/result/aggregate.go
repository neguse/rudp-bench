package result

import (
	"fmt"
	"math"
	"sort"
	"strconv"
)

// MetricFormat maps metric column names to their decimal places in the
// emitted median value. Matches Python METRIC_FORMAT.
var MetricFormat = map[string]int{
	"delivery_ratio":              4,
	"forward_delivery_ratio":      4,
	"forward_delivery_ratio_r":    4,
	"forward_delivery_ratio_u":    4,
	"server_echo_accept_ratio":    4,
	"server_echo_accept_ratio_r":  4,
	"server_echo_accept_ratio_u":  4,
	"return_delivery_ratio":       4,
	"return_delivery_ratio_r":     4,
	"return_delivery_ratio_u":     4,
	"server_cpu_pct":              2,
	"server_cpu_pct_peak":         2,
	"rtt_r_p50_us":                0,
	"rtt_r_p95_us":                0,
	"rtt_r_p99_us":                0,
	"rtt_u_p50_us":                0,
	"rtt_u_p95_us":                0,
	"rtt_u_p99_us":                0,
}

// metricCols is the ordered list of metric columns for output.
var metricCols = []string{
	"delivery_ratio",
	"forward_delivery_ratio",
	"forward_delivery_ratio_r",
	"forward_delivery_ratio_u",
	"server_echo_accept_ratio",
	"server_echo_accept_ratio_r",
	"server_echo_accept_ratio_u",
	"return_delivery_ratio",
	"return_delivery_ratio_r",
	"return_delivery_ratio_u",
	"server_cpu_pct",
	"server_cpu_pct_peak",
	"rtt_r_p50_us",
	"rtt_r_p95_us",
	"rtt_r_p99_us",
	"rtt_u_p50_us",
	"rtt_u_p95_us",
	"rtt_u_p99_us",
}

// fnum parses a string to float64, returning (value, true) on success.
func fnum(s string) (float64, bool) {
	if s == "" {
		return 0, false
	}
	v, err := strconv.ParseFloat(s, 64)
	if err != nil {
		return 0, false
	}
	return v, true
}

// median computes the median of a float64 slice.
// Returns (median, true) if vals is non-empty, (0, false) otherwise.
func median(vals []float64) (float64, bool) {
	if len(vals) == 0 {
		return 0, false
	}
	sorted := make([]float64, len(vals))
	copy(sorted, vals)
	sort.Float64s(sorted)
	n := len(sorted)
	if n%2 == 1 {
		return sorted[n/2], true
	}
	return (sorted[n/2-1] + sorted[n/2]) / 2.0, true
}

// medianStr computes the median of non-nil float values and formats with
// the given number of decimal places. Returns "" if no valid values.
func medianStr(values []float64, places int) string {
	if len(values) == 0 {
		return ""
	}
	m, ok := median(values)
	if !ok {
		return ""
	}
	if places == 0 {
		return strconv.Itoa(int(math.Round(m)))
	}
	return fmt.Sprintf("%.*f", places, m)
}

// dominant returns the most frequent non-empty string in a slice.
func dominant(values []string) string {
	counts := map[string]int{}
	for _, v := range values {
		if v != "" {
			counts[v]++
		}
	}
	if len(counts) == 0 {
		return ""
	}
	var best string
	bestCount := 0
	for v, c := range counts {
		if c > bestCount {
			bestCount = c
			best = v
		}
	}
	return best
}

// loadConns builds a map of (library, scenario_id) -> conns from a scenarios CSV.
func loadConns(scenariosPath string) map[[2]string]string {
	out := map[[2]string]string{}
	if scenariosPath == "" {
		return out
	}
	rows, err := ReadCSVRows(scenariosPath)
	if err != nil || rows == nil {
		return out
	}
	for _, r := range rows {
		key := [2]string{r["library"], r["scenario_id"]}
		out[key] = r["conns"]
	}
	return out
}

// Aggregate groups result rows by (library, scenario_id), selects valid rows,
// computes median of each metric column, and writes the output CSV.
func Aggregate(resultsPath, scenariosPath, outPath string, minValid int) error {
	connsMap := loadConns(scenariosPath)

	allRows, err := ReadCSVRows(resultsPath)
	if err != nil {
		return fmt.Errorf("reading results: %w", err)
	}

	// Group by (library, scenario_id).
	type groupKey = [2]string
	groups := map[groupKey][]map[string]string{}
	var groupOrder []groupKey
	for _, r := range allRows {
		key := groupKey{r["library"], r["scenario_id"]}
		if _, exists := groups[key]; !exists {
			groupOrder = append(groupOrder, key)
		}
		groups[key] = append(groups[key], r)
	}
	sort.Slice(groupOrder, func(i, j int) bool {
		if groupOrder[i][0] != groupOrder[j][0] {
			return groupOrder[i][0] < groupOrder[j][0]
		}
		return groupOrder[i][1] < groupOrder[j][1]
	})

	// Build output fields.
	fields := []string{"library", "scenario_id", "conns", "n_total", "n_valid", "valid"}
	for _, c := range metricCols {
		fields = append(fields, c+"_median")
	}
	fields = append(fields, "included_run_ids", "note")

	var outRows []map[string]string
	for _, key := range groupOrder {
		rows := groups[key]
		lib, sid := key[0], key[1]

		var validRows []map[string]string
		for _, r := range rows {
			if r["valid"] == "1" {
				validRows = append(validRows, r)
			}
		}

		agg := map[string]string{
			"library":     lib,
			"scenario_id": sid,
			"conns":       connsMap[[2]string{lib, sid}],
			"n_total":     strconv.Itoa(len(rows)),
			"n_valid":     strconv.Itoa(len(validRows)),
		}
		if len(validRows) >= minValid {
			agg["valid"] = "1"
		} else {
			agg["valid"] = "0"
		}

		// included_run_ids
		var runIDs []string
		for _, r := range validRows {
			runIDs = append(runIDs, r["run_id"])
		}
		agg["included_run_ids"] = joinSemicolon(runIDs)

		// note: empty if there are valid rows, otherwise the dominant invalid_reason
		if len(validRows) > 0 {
			agg["note"] = ""
		} else {
			var reasons []string
			for _, r := range rows {
				reasons = append(reasons, r["invalid_reason"])
			}
			agg["note"] = dominant(reasons)
		}

		// Compute medians for each metric column.
		for _, col := range metricCols {
			places := MetricFormat[col]
			var vals []float64
			for _, r := range validRows {
				if v, ok := fnum(r[col]); ok {
					vals = append(vals, v)
				}
			}
			agg[col+"_median"] = medianStr(vals, places)
		}

		outRows = append(outRows, agg)
	}

	return WriteCSV(outPath, fields, outRows)
}
