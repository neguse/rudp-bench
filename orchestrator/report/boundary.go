package report

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/neguse/rudp-bench/orchestrator/boundary"
)

// BoundaryData は boundary sweep 出力(results.jsonl)の読み込み結果。
type BoundaryData struct {
	// anchor → load label → "loss|burst" → transport → point
	points map[string]map[string]map[string]map[string]boundary.PointRecord
	loads  map[string][]string // anchor → 出現した load label(floor, q25, ...)
}

func LoadBoundary(dir string) (*BoundaryData, error) {
	f, err := os.Open(filepath.Join(dir, "results.jsonl"))
	if err != nil {
		return nil, err
	}
	defer f.Close()
	bd := &BoundaryData{
		points: map[string]map[string]map[string]map[string]boundary.PointRecord{},
		loads:  map[string][]string{},
	}
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 1<<20), 1<<20)
	for sc.Scan() {
		var rec boundary.PointRecord
		if json.Unmarshal(sc.Bytes(), &rec) != nil {
			continue
		}
		anchor := bd.points[rec.Anchor]
		if anchor == nil {
			anchor = map[string]map[string]map[string]boundary.PointRecord{}
			bd.points[rec.Anchor] = anchor
		}
		load := anchor[rec.Load.Label]
		if load == nil {
			load = map[string]map[string]boundary.PointRecord{}
			anchor[rec.Load.Label] = load
			bd.loads[rec.Anchor] = append(bd.loads[rec.Anchor], rec.Load.Label)
		}
		cellKey := fmt.Sprintf("%g|%g", rec.LossPct, rec.BurstLen)
		if load[cellKey] == nil {
			load[cellKey] = map[string]boundary.PointRecord{}
		}
		load[cellKey][rec.Transport] = rec
	}
	return bd, sc.Err()
}

// Anchors は出現 anchor の一覧(安定順)。
func (bd *BoundaryData) Anchors() []string {
	out := make([]string, 0, len(bd.points))
	for a := range bd.points {
		out = append(out, a)
	}
	sort.Strings(out)
	return out
}

// LoadLabels は anchor の load label 一覧(floor が先頭、以降出現順)。
func (bd *BoundaryData) LoadLabels(anchor string) []string {
	labels := append([]string(nil), bd.loads[anchor]...)
	sort.SliceStable(labels, func(i, j int) bool {
		if labels[i] == "floor" {
			return true
		}
		if labels[j] == "floor" {
			return false
		}
		return labels[i] < labels[j]
	})
	return labels
}

func formatBoundaryCell(rec boundary.PointRecord, found bool) string {
	if !found {
		return "—"
	}
	if rec.Verdict != "VALID" {
		return "inv"
	}
	return fmt.Sprintf("%d/%d", rec.Judgment.StalenessP99/1_000_000, rec.Judgment.FloorStaleNS/1_000_000)
}

// BoundaryTable は anchor × load の loss 平面表(staleness p99 ms / フロア ms)。
func (bd *BoundaryData) BoundaryTable(anchor, loadLabel string) string {
	load := bd.points[anchor][loadLabel]
	var transports []string
	seen := map[string]bool{}
	for _, byTransport := range load {
		for t := range byTransport {
			seen[t] = true
		}
	}
	for _, t := range transportOrder {
		if seen[t] {
			transports = append(transports, t)
		}
	}

	type gridKey struct{ loss, burst float64 }
	var keys []gridKey
	for cellKey := range load {
		var k gridKey
		fmt.Sscanf(cellKey, "%g|%g", &k.loss, &k.burst)
		keys = append(keys, k)
	}
	sort.Slice(keys, func(i, j int) bool {
		if keys[i].loss != keys[j].loss {
			return keys[i].loss < keys[j].loss
		}
		return keys[i].burst < keys[j].burst
	})

	var conns string
	var b strings.Builder
	b.WriteString("| loss% × burst | " + strings.Join(transports, " | ") + " |\n")
	b.WriteString("|---" + strings.Repeat("|---", len(transports)) + "|\n")
	for _, k := range keys {
		cellKey := fmt.Sprintf("%g|%g", k.loss, k.burst)
		b.WriteString(fmt.Sprintf("| %g × %g |", k.loss, k.burst))
		for _, t := range transports {
			rec, ok := load[cellKey][t]
			b.WriteString(" " + formatBoundaryCell(rec, ok) + " |")
			if ok && conns == "" {
				conns = fmt.Sprintf("%d", rec.Load.Conns)
			}
		}
		b.WriteString("\n")
	}
	b.WriteString("\n*セル = staleness p99 ms / 物理フロア ms(フロア = 遅延+バースト黒塗り+間隔+サンプル周期)。" +
		"`inv` = validity gate 不成立。負荷 = " + loadLabel + "(conns は transport ごとに capacity@wired 比で決まる)。*\n")
	return b.String()
}
