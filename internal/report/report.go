package report

import (
	"encoding/csv"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"
)

var scenarioRE = regexp.MustCompile(`^(.+)_r(\d+)_u(\d+)_(\d+)_(\d+)_(echo|broadcast)_([^_]+)_(.+)$`)

// readCSV reads a CSV file into a slice of maps keyed by header names.
func readCSV(path string) ([]map[string]string, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()

	r := csv.NewReader(f)
	records, err := r.ReadAll()
	if err != nil {
		return nil, err
	}
	if len(records) == 0 {
		return nil, nil
	}
	headers := records[0]
	var rows []map[string]string
	for _, rec := range records[1:] {
		row := make(map[string]string, len(headers))
		for i, h := range headers {
			if i < len(rec) {
				row[h] = rec[i]
			}
		}
		rows = append(rows, row)
	}
	return rows, nil
}

func toFloat(s string) (float64, bool) {
	if s == "" {
		return 0, false
	}
	v, err := strconv.ParseFloat(s, 64)
	if err != nil || math.IsNaN(v) {
		return 0, false
	}
	return v, true
}

func toInt(s string) (int, bool) {
	v, ok := toFloat(s)
	if !ok {
		return 0, false
	}
	return int(v), true
}

func profileOrder(profileRows []map[string]string) []string {
	var out []string
	for _, row := range profileRows {
		p := row["profile"]
		if p != "" {
			out = append(out, p)
		}
	}
	return out
}

type profileKey struct {
	mode, rateR, rateU, size string
}

func loadProfiles(runDir string) ([]map[string]string, map[profileKey]string) {
	path := filepath.Join(runDir, "profiles.csv")
	rows, err := readCSV(path)
	if err != nil || len(rows) == 0 {
		rows = defaultProfileRows()
	}
	keyMap := make(map[profileKey]string)
	for _, row := range rows {
		k := profileKey{
			mode:  row["mode"],
			rateR: row["rate_r"],
			rateU: row["rate_u"],
			size:  row["size"],
		}
		keyMap[k] = row["profile"]
	}
	return rows, keyMap
}

func defaultProfileRows() []map[string]string {
	return []map[string]string{
		{"profile": "media_relay", "use_case": "media_sfu_unreliable_fanout", "mode": "broadcast", "rate_r": "0", "rate_u": "30", "size": "1000", "conns_schedule": "1 5 50 75 100 125 150 200", "client_procs": "4", "notes": "near-MTU media packets, full-room unreliable fanout"},
		{"profile": "game_server", "use_case": "authoritative_game_snapshot_event_fanout", "mode": "broadcast", "rate_r": "1", "rate_u": "20", "size": "128", "conns_schedule": "1 5 64 96 128 192 256", "client_procs": "4", "notes": "20Hz state/input fanout plus 1Hz reliable gameplay events"},
		{"profile": "reliable_echo", "use_case": "reliable_transport_echo_baseline", "mode": "echo", "rate_r": "50", "rate_u": "0", "size": "64", "conns_schedule": "1 50 200 600 1000 1500 2000 3000", "client_procs": "8", "notes": "reliable-only echo baseline for stream/reliable transports"},
		{"profile": "echo", "use_case": "synthetic_mixed_echo_baseline", "mode": "echo", "rate_r": "50", "rate_u": "50", "size": "64", "conns_schedule": "1 50 200 600 1000 1500 2000 3000", "client_procs": "8", "notes": "mixed 50/50 echo baseline used for implementation validation"},
	}
}

func annotatedRows(summaryRows []map[string]string, profileByShape map[profileKey]string) []AnnotatedRow {
	var out []AnnotatedRow
	for _, row := range summaryRows {
		ar := AnnotatedRow{
			Library: row["library"],
			Values:  row,
		}
		m := scenarioRE.FindStringSubmatch(row["scenario_id"])
		if m != nil {
			k := profileKey{
				mode:  m[6],
				rateR: m[2],
				rateU: m[3],
				size:  m[4],
			}
			ar.Profile = profileByShape[k]
			if c, ok := toInt(m[5]); ok {
				ar.Conns = c
			}
		}
		if ar.Profile == "" {
			ar.Profile = ""
		}
		if ar.Conns == 0 {
			if c, ok := toInt(row["conns"]); ok {
				ar.Conns = c
			}
		}
		out = append(out, ar)
	}
	return out
}

func libraries(capacityRows []map[string]string, summaryRows []map[string]string) []string {
	seen := make(map[string]bool)
	var out []string
	for _, rows := range [][]map[string]string{capacityRows, summaryRows} {
		for _, row := range rows {
			lib := row["library"]
			if lib != "" && !seen[lib] {
				seen[lib] = true
				out = append(out, lib)
			}
		}
	}
	return out
}

func displayLastOK(row map[string]string) string {
	v := row["last_ok_conns"]
	if v != "" {
		return v
	}
	status := row["status"]
	switch status {
	case "unsupported", "below_gate", "failed_gate":
		return status
	}
	return "unmeasured"
}

func strongestRows(capacityRows []map[string]string, profiles []string) [][]string {
	var out [][]string
	for _, profile := range profiles {
		var best map[string]string
		bestOK := -1
		bestCPU := math.Inf(1)

		for _, row := range capacityRows {
			if row["profile"] != profile {
				continue
			}
			ok, valid := toInt(row["last_ok_conns"])
			if !valid {
				continue
			}
			cpu := 999999.0
			if c, cOk := toFloat(row["last_ok_server_cpu"]); cOk {
				cpu = c
			}
			if ok > bestOK || (ok == bestOK && cpu < bestCPU) {
				best = row
				bestOK = ok
				bestCPU = cpu
			}
		}

		if best == nil {
			out = append(out, []string{profile, "unmeasured", "unmeasured", "unmeasured", ""})
			continue
		}

		breakText := "not broken"
		if best["break_conns"] != "" {
			breakText = best["break_conns"] + " (" + best["break_reason"] + ")"
		}

		readout := fmt.Sprintf("delivery %s, CPU %s%%", best["last_ok_delivery"], best["last_ok_server_cpu"])
		out = append(out, []string{profile, best["library"], best["last_ok_conns"], breakText, readout})
	}
	return out
}

func markdownTable(headers []string, rows [][]string) string {
	escCell := func(s string) string {
		return strings.ReplaceAll(s, "|", "\\|")
	}

	var lines []string
	hCells := make([]string, len(headers))
	for i, h := range headers {
		hCells[i] = escCell(h)
	}
	lines = append(lines, "| "+strings.Join(hCells, " | ")+" |")

	seps := make([]string, len(headers))
	for i := range headers {
		seps[i] = "---"
	}
	lines = append(lines, "| "+strings.Join(seps, " | ")+" |")

	for _, row := range rows {
		cells := make([]string, len(headers))
		for i := range headers {
			if i < len(row) {
				cells[i] = escCell(row[i])
			}
		}
		lines = append(lines, "| "+strings.Join(cells, " | ")+" |")
	}
	return strings.Join(lines, "\n")
}

func rttP95Ms(row AnnotatedRow) (float64, bool) {
	unreliable, uOk := toFloat(row.Values["rtt_u_p95_us_median"])
	reliable, rOk := toFloat(row.Values["rtt_r_p95_us_median"])
	var chosen float64
	var ok bool
	if uOk && unreliable > 0 {
		chosen = unreliable
		ok = true
	} else if rOk {
		chosen = reliable
		ok = true
	}
	if !ok {
		return 0, false
	}
	return chosen / 1000.0, true
}

func relPath(target, base string) string {
	absT, err := filepath.Abs(target)
	if err != nil {
		return target
	}
	absB, err := filepath.Abs(base)
	if err != nil {
		return target
	}
	r, err := filepath.Rel(absB, absT)
	if err != nil {
		return target
	}
	return filepath.ToSlash(r)
}

// Render generates the full benchmark report with SVG plots.
func Render(runDir, outPath, plotsDir, runLabel string) error {
	if outPath == "" {
		outPath = filepath.Join(runDir, "report.md")
	}
	if plotsDir == "" {
		plotsDir = filepath.Join(runDir, "plots")
	}
	if runLabel == "" {
		abs, err := filepath.Abs(runDir)
		if err != nil {
			runLabel = runDir
		} else {
			runLabel = abs
		}
	}

	// Read input CSVs
	capacityRows, err := readCSV(filepath.Join(runDir, "capacity.csv"))
	if err != nil {
		return fmt.Errorf("reading capacity.csv: %w", err)
	}
	summaryRowsRaw, err := readCSV(filepath.Join(runDir, "summary.csv"))
	if err != nil {
		return fmt.Errorf("reading summary.csv: %w", err)
	}

	profileRows, profileByShape := loadProfiles(runDir)
	profiles := profileOrder(profileRows)
	summaryAnnotated := annotatedRows(summaryRowsRaw, profileByShape)
	libs := libraries(capacityRows, summaryRowsRaw)

	// Generate plots
	if err := os.MkdirAll(plotsDir, 0755); err != nil {
		return fmt.Errorf("creating plots dir: %w", err)
	}

	type plotEntry struct {
		title string
		path  string
	}
	topPlots := []plotEntry{}
	profilePlots := map[string][]plotEntry{}

	// Capacity bar chart
	capacityPath := filepath.Join(plotsDir, "capacity_max_ok.svg")
	if err := SaveCapacityPlot(capacityRows, profiles, libs, capacityPath); err != nil {
		return fmt.Errorf("saving capacity plot: %w", err)
	}
	if _, err := os.Stat(capacityPath); err == nil {
		topPlots = append(topPlots, plotEntry{"Max OK capacity", capacityPath})
	}

	// Per-profile metric plots
	for _, profile := range profiles {
		var pPlots []plotEntry

		type metricDef struct {
			suffix string
			opts   MetricPlotOpts
		}
		metrics := []metricDef{
			{
				suffix: "_delivery.svg",
				opts: MetricPlotOpts{
					ValueFn: func(row AnnotatedRow) (float64, bool) {
						return toFloat(row.Values["delivery_ratio_median"])
					},
					YLabel:    "median delivery ratio",
					Title:     profile + ": delivery ratio vs connections",
					Threshold: 0.95,
				},
			},
			{
				suffix: "_server_cpu.svg",
				opts: MetricPlotOpts{
					ValueFn: func(row AnnotatedRow) (float64, bool) {
						return toFloat(row.Values["server_cpu_pct_median"])
					},
					YLabel: "median server CPU %",
					Title:  profile + ": server CPU vs connections",
				},
			},
			{
				suffix: "_rtt_p95.svg",
				opts: MetricPlotOpts{
					ValueFn: rttP95Ms,
					YLabel:  "median RTT p95 (ms)",
					Title:   profile + ": RTT p95 vs connections",
				},
			},
		}

		for _, md := range metrics {
			plotPath := filepath.Join(plotsDir, profile+md.suffix)
			if err := SaveMetricPlot(summaryAnnotated, profile, libs, md.opts, plotPath); err != nil {
				return fmt.Errorf("saving %s plot: %w", profile+md.suffix, err)
			}
			if _, err := os.Stat(plotPath); err == nil {
				title := strings.ReplaceAll(strings.TrimSuffix(filepath.Base(plotPath), ".svg"), "_", " ")
				pPlots = append(pPlots, plotEntry{title, plotPath})
			}
		}
		profilePlots[profile] = pPlots
	}

	// Build markdown report
	outDir := filepath.Dir(outPath)
	if err := os.MkdirAll(outDir, 0755); err != nil {
		return fmt.Errorf("creating output dir: %w", err)
	}

	generated := time.Now().UTC().Format("2006-01-02 15:04:05 UTC")
	strongest := strongestRows(capacityRows, profiles)

	var lines []string
	lines = append(lines,
		"# Canonical Benchmark Report",
		"",
		"Generated: "+generated,
		"",
		"Result directory: `"+runLabel+"`",
		"",
		"This report is generated by `go run ./cmd/rudp-bench-canonical`. It is the first file to open after a canonical benchmark run.",
		"",
		"## Verdict",
		"",
		markdownTable(
			[]string{"profile", "strongest", "max OK", "break", "max OK readout"},
			strongest,
		),
		"",
		"OK means aggregate valid runs meet the gate and median `delivery_ratio >= 0.95`.",
		"",
		"## Graphs",
		"",
	)

	for _, p := range topPlots {
		lines = append(lines, fmt.Sprintf("![%s](%s)", p.title, relPath(p.path, outDir)), "")
	}

	for _, profile := range profiles {
		lines = append(lines, fmt.Sprintf("### `%s`", profile), "")
		pp := profilePlots[profile]
		if len(pp) == 0 {
			lines = append(lines, "No graphable rows.", "")
			continue
		}
		for _, p := range pp {
			lines = append(lines, fmt.Sprintf("![%s](%s)", p.title, relPath(p.path, outDir)), "")
		}
	}

	// Capacity table
	var capTableRows [][]string
	for _, row := range capacityRows {
		breakConns := row["break_conns"]
		if breakConns == "" {
			breakConns = "not broken"
		}
		capTableRows = append(capTableRows, []string{
			row["profile"],
			row["library"],
			row["status"],
			displayLastOK(row),
			row["last_ok_delivery"],
			row["last_ok_server_cpu"],
			breakConns,
			row["break_reason"],
			row["break_delivery"],
			row["break_server_cpu"],
		})
	}
	lines = append(lines,
		"## Capacity Table",
		"",
		markdownTable(
			[]string{"profile", "library", "status", "last OK", "last OK delivery", "last OK CPU", "break", "break reason", "break delivery", "break CPU"},
			capTableRows,
		),
		"",
	)

	// Profiles table
	var profTableRows [][]string
	for _, row := range profileRows {
		profTableRows = append(profTableRows, []string{
			row["profile"],
			row["mode"],
			"r" + row["rate_r"] + "/u" + row["rate_u"],
			row["size"],
			row["conns_schedule"],
			row["client_procs"],
		})
	}
	lines = append(lines,
		"## Profiles",
		"",
		markdownTable(
			[]string{"profile", "mode", "traffic", "payload", "conn sweep", "client procs"},
			profTableRows,
		),
		"",
	)

	// Data files
	dataFiles := []string{"capacity.csv", "summary.csv", "profiles.csv"}
	for _, opt := range []string{"results_all.csv", "scenarios_all.csv"} {
		if _, err := os.Stat(filepath.Join(runDir, opt)); err == nil {
			dataFiles = append(dataFiles, opt)
		}
	}
	lines = append(lines, "## Data Files", "")
	for _, name := range dataFiles {
		r := relPath(filepath.Join(runDir, name), outDir)
		lines = append(lines, fmt.Sprintf("- [`%s`](%s)", name, r))
	}
	lines = append(lines, "")

	content := strings.Join(lines, "\n")
	if err := os.WriteFile(outPath, []byte(content), 0644); err != nil {
		return fmt.Errorf("writing report: %w", err)
	}

	fmt.Println("wrote", outPath)
	for _, p := range topPlots {
		fmt.Println("wrote", p.path)
	}
	for _, profile := range profiles {
		for _, p := range profilePlots[profile] {
			fmt.Println("wrote", p.path)
		}
	}
	return nil
}
