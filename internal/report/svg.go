package report

import (
	"fmt"
	"math"
	"os"
	"sort"
	"strconv"
	"strings"
)

var colorPalette = [14]string{
	"#1f77b4", "#ff7f0e", "#2ca02c", "#d62728", "#9467bd",
	"#8c564b", "#e377c2", "#7f7f7f", "#bcbd22", "#17becf",
	"#aec7e8", "#ffbb78", "#98df8a", "#ff9896",
}

// autoTicks generates nice tick values (multiples of 1, 2, 5, 10, 20, 50, etc.).
func autoTicks(min, max float64, maxTicks int) []float64 {
	if maxTicks < 2 {
		maxTicks = 2
	}
	span := max - min
	if span <= 0 {
		return []float64{min}
	}
	rawStep := span / float64(maxTicks-1)
	mag := math.Pow(10, math.Floor(math.Log10(rawStep)))
	normalized := rawStep / mag
	var step float64
	switch {
	case normalized <= 1:
		step = mag
	case normalized <= 2:
		step = 2 * mag
	case normalized <= 5:
		step = 5 * mag
	default:
		step = 10 * mag
	}
	start := math.Floor(min/step) * step
	var ticks []float64
	for v := start; v <= max+step*0.01; v += step {
		if v >= min-step*0.01 {
			ticks = append(ticks, v)
		}
	}
	if len(ticks) == 0 {
		ticks = []float64{min, max}
	}
	return ticks
}

// AnnotatedRow holds a summary row with parsed profile and connection count.
type AnnotatedRow struct {
	Library string
	Profile string
	Conns   int
	Values  map[string]string
}

// MetricPlotOpts configures a line-chart metric plot.
type MetricPlotOpts struct {
	ValueFn   func(row AnnotatedRow) (float64, bool)
	YLabel    string
	Title     string
	Threshold float64
}

func escSVG(s string) string {
	s = strings.ReplaceAll(s, "&", "&amp;")
	s = strings.ReplaceAll(s, "<", "&lt;")
	s = strings.ReplaceAll(s, ">", "&gt;")
	s = strings.ReplaceAll(s, "\"", "&quot;")
	return s
}

func formatTickLabel(v float64) string {
	if v == math.Trunc(v) && math.Abs(v) < 1e12 {
		return strconv.FormatInt(int64(v), 10)
	}
	// Use enough precision but trim trailing zeros
	s := strconv.FormatFloat(v, 'f', 10, 64)
	if strings.Contains(s, ".") {
		s = strings.TrimRight(s, "0")
		s = strings.TrimRight(s, ".")
	}
	return s
}

// SaveCapacityPlot generates a horizontal bar chart SVG for capacity overview.
func SaveCapacityPlot(capacityRows []map[string]string, profiles, libs []string, outPath string) error {
	if len(capacityRows) == 0 {
		return nil
	}

	const (
		viewW       = 760
		marginLeft  = 130
		marginRight = 80
		marginTop   = 10
		subplotH    = 200
		barPad      = 4
		titleH      = 24
	)

	totalH := marginTop + len(profiles)*subplotH + 10

	var sb strings.Builder
	sb.WriteString(fmt.Sprintf(`<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" font-family="sans-serif" font-size="11">`, viewW, totalH))
	sb.WriteString("\n")

	plotW := float64(viewW - marginLeft - marginRight)

	for pi, profile := range profiles {
		subY := float64(marginTop + pi*subplotH)
		barAreaH := float64(subplotH - titleH - 20)
		barH := barAreaH / float64(len(libs))
		if barH > 30 {
			barH = 30
		}

		// Collect values
		type barData struct {
			lib    string
			value  float64
			label  string
			status string
		}
		var bars []barData
		xmax := 0.0
		for _, lib := range libs {
			var row map[string]string
			for _, r := range capacityRows {
				if r["profile"] == profile && r["library"] == lib {
					row = r
					break
				}
			}
			val := 0.0
			label := "unmeasured"
			status := ""
			if row != nil {
				status = row["status"]
				if status == "unsupported" || status == "below_gate" || status == "failed_gate" {
					label = status
				} else if v, err := strconv.ParseFloat(row["last_ok_conns"], 64); err == nil && v > 0 {
					val = v
					label = strconv.Itoa(int(v))
				}
			}
			bars = append(bars, barData{lib: lib, value: val, label: label, status: status})
			if val > xmax {
				xmax = val
			}
		}
		if xmax <= 0 {
			xmax = 1
		}
		xlim := xmax * 1.18
		scale := plotW / xlim

		// Profile title
		sb.WriteString(fmt.Sprintf(`  <text x="%d" y="%.0f" font-size="13" font-weight="bold">%s: max OK connections</text>`,
			marginLeft, subY+16, escSVG(profile)))
		sb.WriteString("\n")

		barsTop := subY + float64(titleH)

		// Gridlines
		ticks := autoTicks(0, xmax, 6)
		for _, tv := range ticks {
			gx := float64(marginLeft) + tv*scale
			sb.WriteString(fmt.Sprintf(`  <line x1="%.1f" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#ddd" stroke-width="1"/>`,
				gx, barsTop, gx, barsTop+barH*float64(len(libs))))
			sb.WriteString("\n")
			// Tick label
			sb.WriteString(fmt.Sprintf(`  <text x="%.1f" y="%.1f" text-anchor="middle" font-size="9" fill="#666">%s</text>`,
				gx, barsTop+barH*float64(len(libs))+12, formatTickLabel(tv)))
			sb.WriteString("\n")
		}

		for bi, bd := range bars {
			by := barsTop + float64(bi)*barH + float64(barPad)/2
			bh := barH - float64(barPad)
			bw := bd.value * scale
			color := colorPalette[bi%len(colorPalette)]

			// Library label (y-axis)
			textY := by + bh/2 + 4
			sb.WriteString(fmt.Sprintf(`  <text x="%d" y="%.1f" text-anchor="end" font-size="10">%s</text>`,
				marginLeft-6, textY, escSVG(bd.lib)))
			sb.WriteString("\n")

			if bw > 0 {
				sb.WriteString(fmt.Sprintf(`  <rect x="%d" y="%.1f" width="%.1f" height="%.1f" fill="%s" rx="2">`,
					marginLeft, by, bw, bh, color))
				sb.WriteString(fmt.Sprintf(`<title>%s: %s (%s)</title></rect>`,
					escSVG(bd.lib), escSVG(bd.label), escSVG(profile)))
				sb.WriteString("\n")
			}

			// Value label
			var labelX float64
			if bw > 0 {
				labelX = float64(marginLeft) + bw + math.Max(xmax*scale*0.02, 4)
			} else {
				labelX = float64(marginLeft) + math.Max(xmax*scale*0.02, 4)
			}
			sb.WriteString(fmt.Sprintf(`  <text x="%.1f" y="%.1f" font-size="9">%s</text>`,
				labelX, textY, escSVG(bd.label)))
			sb.WriteString("\n")
		}
	}

	sb.WriteString("</svg>\n")

	return os.WriteFile(outPath, []byte(sb.String()), 0644)
}

// SaveMetricPlot generates a line chart SVG for a metric vs connections.
func SaveMetricPlot(rows []AnnotatedRow, profile string, libs []string, opts MetricPlotOpts, outPath string) error {
	const (
		viewW        = 640
		viewH        = 400
		marginLeft   = 65
		marginRight  = 20
		marginTop    = 35
		marginBottom = 90 // extra room for legend
	)

	plotW := float64(viewW - marginLeft - marginRight)
	plotH := float64(viewH - marginTop - marginBottom)

	// Collect data per library
	type series struct {
		lib string
		xs  []float64
		ys  []float64
	}
	var allSeries []series
	globalXMin, globalXMax := math.Inf(1), math.Inf(-1)
	globalYMin, globalYMax := math.Inf(1), math.Inf(-1)

	for _, lib := range libs {
		type pt struct {
			x, y float64
		}
		var points []pt
		for _, row := range rows {
			if row.Profile != profile || row.Library != lib {
				continue
			}
			v, ok := opts.ValueFn(row)
			if !ok {
				continue
			}
			points = append(points, pt{float64(row.Conns), v})
		}
		if len(points) == 0 {
			continue
		}
		sort.Slice(points, func(i, j int) bool { return points[i].x < points[j].x })
		s := series{lib: lib}
		for _, p := range points {
			s.xs = append(s.xs, p.x)
			s.ys = append(s.ys, p.y)
			if p.x < globalXMin {
				globalXMin = p.x
			}
			if p.x > globalXMax {
				globalXMax = p.x
			}
			if p.y < globalYMin {
				globalYMin = p.y
			}
			if p.y > globalYMax {
				globalYMax = p.y
			}
		}
		allSeries = append(allSeries, s)
	}

	if len(allSeries) == 0 {
		return nil
	}

	// Include threshold in Y range if set
	if opts.Threshold > 0 {
		if opts.Threshold < globalYMin {
			globalYMin = opts.Threshold
		}
		if opts.Threshold > globalYMax {
			globalYMax = opts.Threshold
		}
	}

	// Add padding to ranges
	if globalXMax == globalXMin {
		globalXMax = globalXMin + 1
	}
	ySpan := globalYMax - globalYMin
	if ySpan <= 0 {
		ySpan = 1
	}
	globalYMin -= ySpan * 0.05
	globalYMax += ySpan * 0.05

	xTicks := autoTicks(globalXMin, globalXMax, 8)
	yTicks := autoTicks(globalYMin, globalYMax, 6)

	// Adjust range to include all ticks
	if len(xTicks) > 0 {
		if xTicks[0] < globalXMin {
			globalXMin = xTicks[0]
		}
		if xTicks[len(xTicks)-1] > globalXMax {
			globalXMax = xTicks[len(xTicks)-1]
		}
	}
	if len(yTicks) > 0 {
		if yTicks[0] < globalYMin {
			globalYMin = yTicks[0]
		}
		if yTicks[len(yTicks)-1] > globalYMax {
			globalYMax = yTicks[len(yTicks)-1]
		}
	}

	xScale := plotW / (globalXMax - globalXMin)
	yScale := plotH / (globalYMax - globalYMin)

	toSX := func(v float64) float64 { return float64(marginLeft) + (v-globalXMin)*xScale }
	toSY := func(v float64) float64 { return float64(marginTop) + plotH - (v-globalYMin)*yScale }

	var sb strings.Builder
	sb.WriteString(fmt.Sprintf(`<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" font-family="sans-serif" font-size="11">`, viewW, viewH))
	sb.WriteString("\n")

	// Title
	sb.WriteString(fmt.Sprintf(`  <text x="%.0f" y="18" text-anchor="middle" font-size="14" font-weight="bold">%s</text>`,
		float64(marginLeft)+plotW/2, escSVG(opts.Title)))
	sb.WriteString("\n")

	// Plot area background
	sb.WriteString(fmt.Sprintf(`  <rect x="%d" y="%d" width="%.0f" height="%.0f" fill="white" stroke="#ccc"/>`,
		marginLeft, marginTop, plotW, plotH))
	sb.WriteString("\n")

	// Y-axis gridlines and labels
	for _, tv := range yTicks {
		sy := toSY(tv)
		if sy < float64(marginTop) || sy > float64(marginTop)+plotH {
			continue
		}
		sb.WriteString(fmt.Sprintf(`  <line x1="%d" y1="%.1f" x2="%.1f" y2="%.1f" stroke="#ddd" stroke-width="1"/>`,
			marginLeft, sy, float64(marginLeft)+plotW, sy))
		sb.WriteString("\n")
		sb.WriteString(fmt.Sprintf(`  <text x="%d" y="%.1f" text-anchor="end" font-size="9" fill="#333">%s</text>`,
			marginLeft-5, sy+3, formatTickLabel(tv)))
		sb.WriteString("\n")
	}

	// X-axis gridlines and labels
	for _, tv := range xTicks {
		sx := toSX(tv)
		if sx < float64(marginLeft) || sx > float64(marginLeft)+plotW {
			continue
		}
		sb.WriteString(fmt.Sprintf(`  <line x1="%.1f" y1="%d" x2="%.1f" y2="%.1f" stroke="#ddd" stroke-width="1"/>`,
			sx, marginTop, sx, float64(marginTop)+plotH))
		sb.WriteString("\n")
		sb.WriteString(fmt.Sprintf(`  <text x="%.1f" y="%.1f" text-anchor="middle" font-size="9" fill="#333">%s</text>`,
			sx, float64(marginTop)+plotH+14, formatTickLabel(tv)))
		sb.WriteString("\n")
	}

	// Axis labels
	sb.WriteString(fmt.Sprintf(`  <text x="%.0f" y="%.0f" text-anchor="middle" font-size="11" fill="#333">connections</text>`,
		float64(marginLeft)+plotW/2, float64(marginTop)+plotH+30))
	sb.WriteString("\n")
	sb.WriteString(fmt.Sprintf(`  <text x="14" y="%.0f" text-anchor="middle" font-size="11" fill="#333" transform="rotate(-90,14,%.0f)">%s</text>`,
		float64(marginTop)+plotH/2, float64(marginTop)+plotH/2, escSVG(opts.YLabel)))
	sb.WriteString("\n")

	// Threshold line
	if opts.Threshold > 0 {
		ty := toSY(opts.Threshold)
		if ty >= float64(marginTop) && ty <= float64(marginTop)+plotH {
			sb.WriteString(fmt.Sprintf(`  <line x1="%d" y1="%.1f" x2="%.1f" y2="%.1f" stroke="black" stroke-width="1.2" stroke-dasharray="6,4" opacity="0.55"/>`,
				marginLeft, ty, float64(marginLeft)+plotW, ty))
			sb.WriteString("\n")
			sb.WriteString(fmt.Sprintf(`  <text x="%.1f" y="%.1f" text-anchor="end" font-size="9" fill="#333"> %s</text>`,
				float64(marginLeft)+plotW-2, ty-3, formatTickLabel(opts.Threshold)))
			sb.WriteString("\n")
		}
	}

	// Data series
	for si, s := range allSeries {
		color := colorPalette[si%len(colorPalette)]

		// Polyline
		var pts []string
		for i := range s.xs {
			pts = append(pts, fmt.Sprintf("%.1f,%.1f", toSX(s.xs[i]), toSY(s.ys[i])))
		}
		sb.WriteString(fmt.Sprintf(`  <polyline points="%s" fill="none" stroke="%s" stroke-width="1.8"/>`,
			strings.Join(pts, " "), color))
		sb.WriteString("\n")

		// Circle markers with tooltips
		for i := range s.xs {
			sx := toSX(s.xs[i])
			sy := toSY(s.ys[i])
			tooltip := fmt.Sprintf("%s: %s at %s connections",
				s.lib, formatTickLabel(s.ys[i]), formatTickLabel(s.xs[i]))
			sb.WriteString(fmt.Sprintf(`  <circle cx="%.1f" cy="%.1f" r="3" fill="%s"><title>%s</title></circle>`,
				sx, sy, color, escSVG(tooltip)))
			sb.WriteString("\n")
		}
	}

	// Legend (2-column layout below chart)
	legendTop := float64(viewH) - float64(marginBottom) + 45
	colW := plotW / 2
	for i, s := range allSeries {
		col := i % 2
		row := i / 2
		lx := float64(marginLeft) + float64(col)*colW
		ly := legendTop + float64(row)*16
		color := colorPalette[i%len(colorPalette)]
		sb.WriteString(fmt.Sprintf(`  <rect x="%.0f" y="%.0f" width="12" height="10" fill="%s" rx="1"/>`,
			lx, ly-8, color))
		sb.WriteString(fmt.Sprintf(`  <text x="%.0f" y="%.0f" font-size="9">%s</text>`,
			lx+16, ly, escSVG(s.lib)))
		sb.WriteString("\n")
	}

	sb.WriteString("</svg>\n")

	return os.WriteFile(outPath, []byte(sb.String()), 0644)
}
