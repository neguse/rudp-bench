package result

import (
	"fmt"
	"strings"

	"github.com/neguse/rudp-bench/internal/bench"
)

// CapacityRow tracks the capacity state for a (profile, library) pair.
type CapacityRow struct {
	Profile        string
	Library        string
	Status         string
	LastOKConns    string
	LastOKDelivery string
	LastOKServerCPU string
	BreakConns     string
	BreakReason    string
	BreakDelivery  string
	BreakServerCPU string
}

// CapacityTracker manages capacity rows keyed by [profile, library].
type CapacityTracker struct {
	rows  map[[2]string]*CapacityRow
	order [][2]string // insertion order for sorted output
}

// NewCapacityTracker creates an empty CapacityTracker.
func NewCapacityTracker() *CapacityTracker {
	return &CapacityTracker{
		rows: make(map[[2]string]*CapacityRow),
	}
}

// ensureCapacityRow returns the CapacityRow for (profile, library), creating it
// if necessary with default values.
func (t *CapacityTracker) ensureCapacityRow(profileName, lib string) *CapacityRow {
	key := [2]string{profileName, lib}
	if row, ok := t.rows[key]; ok {
		return row
	}
	row := &CapacityRow{
		Profile: profileName,
		Library: lib,
		Status:  "not_started",
	}
	t.rows[key] = row
	t.order = append(t.order, key)
	return row
}

// stopReason determines why a library should stop at this connection count.
// Matches Python stop_reason.
func stopReason(row map[string]string, minDelivery float64) string {
	if row == nil {
		return "missing_summary"
	}
	if row["valid"] != "1" {
		note := row["note"]
		if note == "" {
			note = fmt.Sprintf("valid_runs=%s/%s", row["n_valid"], row["n_total"])
		}
		if strings.HasPrefix(note, "unsupported_") {
			return note
		}
		return "aggregate_invalid:" + note
	}
	deliveryStr := row["delivery_ratio_median"]
	delivery, ok := FloatOrNone(deliveryStr)
	if !ok {
		return "missing_delivery"
	}
	if delivery < minDelivery {
		return fmt.Sprintf("delivery<%.2f", minDelivery)
	}
	return "ok"
}

// UnsupportedProfileReason checks if a library is unsupported for a profile
// based on channel support and payload size. Returns the reason string or "".
func UnsupportedProfileReason(profile bench.Profile, lib string) string {
	type channelRate struct {
		channel string
		rate    int
	}
	for _, cr := range []channelRate{{"r", profile.RateR}, {"u", profile.RateU}} {
		if cr.rate <= 0 {
			continue
		}
		if !bench.SupportsReliability(lib, cr.channel) {
			if cr.channel == "r" {
				return "unsupported_reliable"
			}
			return "unsupported_unreliable"
		}
		maxPayload, hasCap := bench.MaxPayloadBytes(lib, cr.channel)
		if hasCap && profile.Size > maxPayload {
			return "unsupported_payload"
		}
	}
	return ""
}

// UnsupportedConns checks if a library's connection limit is exceeded.
func UnsupportedConns(lib string, conns int) bool {
	maxConns, hasCap := bench.MaxConnections(lib)
	return hasCap && conns > maxConns
}

// markFirstMeasuredFailure handles the case where the first measured point
// already fails the gate.
func markFirstMeasuredFailure(cap *CapacityRow, row map[string]string, conns int, reason string) {
	if strings.HasPrefix(reason, "delivery<") {
		cap.Status = "below_gate"
	} else {
		cap.Status = "failed_gate"
	}
	cap.LastOKConns = cap.Status
	cap.BreakConns = fmt.Sprintf("%d", conns)
	cap.BreakReason = reason
	cap.BreakDelivery = row["delivery_ratio_median"]
	cap.BreakServerCPU = row["server_cpu_pct_median"]
}

// UpdateRows updates capacity tracking for all libraries at a given connection
// count. Returns the list of libraries that broke at this point.
func (t *CapacityTracker) UpdateRows(
	profile bench.Profile,
	libs []string,
	summaryRows map[string]map[string]string,
	conns int,
	minDelivery float64,
) []string {
	var broken []string
	for _, lib := range libs {
		row := summaryRows[lib]
		reason := stopReason(row, minDelivery)
		cap := t.ensureCapacityRow(profile.Name, lib)

		if reason == "ok" {
			cap.Status = "not_broken"
			cap.LastOKConns = fmt.Sprintf("%d", conns)
			if row != nil {
				cap.LastOKDelivery = row["delivery_ratio_median"]
				cap.LastOKServerCPU = row["server_cpu_pct_median"]
			}
			continue
		}

		if strings.HasPrefix(reason, "unsupported_") && cap.LastOKConns == "" {
			t.MarkStop(profile, lib, conns, reason)
			broken = append(broken, lib)
			continue
		}

		if cap.LastOKConns == "" && row != nil {
			markFirstMeasuredFailure(cap, row, conns, reason)
			broken = append(broken, lib)
			continue
		}

		cap.Status = "broken"
		cap.BreakConns = fmt.Sprintf("%d", conns)
		cap.BreakReason = reason
		if row != nil {
			cap.BreakDelivery = row["delivery_ratio_median"]
			cap.BreakServerCPU = row["server_cpu_pct_median"]
		}
		broken = append(broken, lib)
	}
	return broken
}

// MarkStop marks a library as stopped at a given connection count with a reason.
func (t *CapacityTracker) MarkStop(profile bench.Profile, lib string, conns int, reason string) {
	cap := t.ensureCapacityRow(profile.Name, lib)
	if strings.HasPrefix(reason, "unsupported_") && cap.LastOKConns == "" {
		cap.Status = "unsupported"
		cap.LastOKConns = "unsupported"
	} else {
		cap.Status = "broken"
	}
	cap.BreakConns = fmt.Sprintf("%d", conns)
	cap.BreakReason = reason
}

// WriteCSV writes all capacity rows to a CSV file.
func (t *CapacityTracker) WriteCSV(path string) error {
	var rows []map[string]string

	// Sort keys for deterministic output.
	keys := make([][2]string, 0, len(t.rows))
	for k := range t.rows {
		keys = append(keys, k)
	}
	sortKeys(keys)

	for _, key := range keys {
		cap := t.rows[key]
		rows = append(rows, map[string]string{
			"profile":            cap.Profile,
			"library":            cap.Library,
			"status":             cap.Status,
			"last_ok_conns":      cap.LastOKConns,
			"last_ok_delivery":   cap.LastOKDelivery,
			"last_ok_server_cpu": cap.LastOKServerCPU,
			"break_conns":        cap.BreakConns,
			"break_reason":       cap.BreakReason,
			"break_delivery":     cap.BreakDelivery,
			"break_server_cpu":   cap.BreakServerCPU,
		})
	}

	return WriteCSV(path, bench.CapacityFields, rows)
}

// PriorCapacity holds the prior measurement state for a (profile, library) pair.
type PriorCapacity struct {
	Status      string
	LastOKConns int
	BreakConns  int
}

// ReadCapacityCSV reads a capacity.csv and returns a map keyed by "profile/library".
func ReadCapacityCSV(path string) (map[string]PriorCapacity, error) {
	rows, err := ReadCSVRows(path)
	if err != nil {
		return nil, err
	}
	out := make(map[string]PriorCapacity, len(rows))
	for _, r := range rows {
		key := r["profile"] + "/" + r["library"]
		pc := PriorCapacity{Status: r["status"]}
		if v, ok := IntOrNone(r["last_ok_conns"]); ok {
			pc.LastOKConns = v
		}
		if v, ok := IntOrNone(r["break_conns"]); ok {
			pc.BreakConns = v
		}
		out[key] = pc
	}
	return out, nil
}

// PriorWasOK returns true if the prior measurement at this (profile, lib, conns)
// was clearly stable (prior existed and conns <= last_ok_conns).
func PriorWasOK(prior map[string]PriorCapacity, profile, lib string, conns int) bool {
	if prior == nil {
		return false
	}
	pc, ok := prior[profile+"/"+lib]
	if !ok {
		return false
	}
	return conns <= pc.LastOKConns
}

// sortKeys sorts [2]string keys lexicographically.
func sortKeys(keys [][2]string) {
	for i := 0; i < len(keys); i++ {
		for j := i + 1; j < len(keys); j++ {
			if keys[j][0] < keys[i][0] || (keys[j][0] == keys[i][0] && keys[j][1] < keys[i][1]) {
				keys[i], keys[j] = keys[j], keys[i]
			}
		}
	}
}
