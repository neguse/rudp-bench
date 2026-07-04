package aggregate

import (
	"bufio"
	"encoding/json"
	"fmt"
	"math/rand"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/neguse/rudp-bench/orchestrator/boundary"
)

// BoundaryKey は 1 セル(anchor × loadLabel × loss × burst × transport)を
// 識別する。
type BoundaryKey struct {
	Anchor    string
	LoadLabel string
	LossPct   float64
	BurstLen  float64
	Transport string
}

// BoundaryAgg はブロック横断で 1 セル(staleness p99)を集約した結果。
// verdict != VALID のブロックは統計(中央値/IQR/CI)から除外するが、
// InvalidN でカウントする(design spec の「unknown=0」の説明責任 — 除外は
// 隠さず可視化する)。
type BoundaryAgg struct {
	N             int      // 統計に使った有効(VALID)ブロック数
	StalenessP99s []uint64 // VALID ブロックの staleness p99(ns、dirs の順序どおり)
	InvalidN      int      // verdict != VALID だったブロック数(統計からは除外・カウントのみ)

	MedianMS float64 // StalenessP99s の中央値(ms)
	IQLoMS   float64 // 25 パーセンタイル(ms)
	IQHiMS   float64 // 75 パーセンタイル(ms)

	CILoMS, CIHiMS float64 // bootstrap median CI(ms)。N < minNForBootstrap では 0
}

// AggregateBoundary は各ブロックの boundary 出力ディレクトリ(results.jsonl
// を持つ)を読み、セルごとに集約する。
func AggregateBoundary(dirs []string) (map[BoundaryKey]BoundaryAgg, error) {
	type raw struct {
		valid bool
		p99   uint64
	}
	byKey := map[BoundaryKey][]raw{}
	var order []BoundaryKey

	for _, dir := range dirs {
		if err := func() error {
			f, err := os.Open(filepath.Join(dir, "results.jsonl"))
			if err != nil {
				return err
			}
			defer f.Close()
			sc := bufio.NewScanner(f)
			sc.Buffer(make([]byte, 1<<20), 1<<20)
			for sc.Scan() {
				var rec boundary.PointRecord
				if json.Unmarshal(sc.Bytes(), &rec) != nil {
					continue // 途中破損行は無視(既存 sweep/boundary と同じ方針)
				}
				key := BoundaryKey{
					Anchor:    rec.Anchor,
					LoadLabel: rec.Load.Label,
					LossPct:   rec.LossPct,
					BurstLen:  rec.BurstLen,
					Transport: rec.Transport,
				}
				if _, ok := byKey[key]; !ok {
					order = append(order, key)
				}
				byKey[key] = append(byKey[key], raw{valid: rec.Verdict == "VALID", p99: rec.Judgment.StalenessP99})
			}
			return sc.Err()
		}(); err != nil {
			return nil, fmt.Errorf("%s: %w", dir, err)
		}
	}

	out := make(map[BoundaryKey]BoundaryAgg, len(byKey))
	rng := rand.New(rand.NewSource(1)) // capacity.go と同じ方針: 固定シードで決定的
	for _, key := range order {
		rows := byKey[key]
		agg := BoundaryAgg{}
		var msValues []float64
		for _, r := range rows {
			if !r.valid {
				agg.InvalidN++
				continue
			}
			agg.StalenessP99s = append(agg.StalenessP99s, r.p99)
			msValues = append(msValues, nsToMS(r.p99))
		}
		agg.N = len(agg.StalenessP99s)
		if agg.N > 0 {
			sorted := sortedCopy(msValues)
			agg.MedianMS = median(sorted)
			agg.IQLoMS = percentile(sorted, 0.25)
			agg.IQHiMS = percentile(sorted, 0.75)
			if agg.N >= minNForBootstrap {
				agg.CILoMS, agg.CIHiMS = BootstrapMedianCI(msValues, bootstrapIters, bootstrapConf, rng)
			}
		}
		out[key] = agg
	}
	return out, nil
}

func nsToMS(ns uint64) float64 { return float64(ns) / 1_000_000 }

func formatBoundaryAgg(a BoundaryAgg, found bool) string {
	if !found {
		return "—"
	}
	if a.N == 0 {
		return fmt.Sprintf("inv(n=%d)", a.InvalidN)
	}
	extra := ""
	if a.InvalidN > 0 {
		extra = fmt.Sprintf(", inv=%d", a.InvalidN)
	}
	return fmt.Sprintf("%s [%s–%s]ms (n=%d%s)", formatNum(a.MedianMS), formatNum(a.IQLoMS), formatNum(a.IQHiMS), a.N, extra)
}

// BoundaryCITable は loss%×burst 行 × transport 列の markdown 表(ブロック
// 集約版)を anchor・loadLabel ごとに生成する。列順・見た目は
// report/boundary.go の BoundaryTable を写す(report パッケージは import しない)。
func BoundaryCITable(aggs map[BoundaryKey]BoundaryAgg, anchor, loadLabel string) string {
	type gridKey struct{ loss, burst float64 }
	present := map[string]bool{}
	gridSeen := map[gridKey]bool{}
	for k := range aggs {
		if k.Anchor != anchor || k.LoadLabel != loadLabel {
			continue
		}
		present[k.Transport] = true
		gridSeen[gridKey{k.LossPct, k.BurstLen}] = true
	}
	var transports []string
	for _, t := range transportOrder {
		if present[t] {
			transports = append(transports, t)
		}
	}
	var grid []gridKey
	for k := range gridSeen {
		grid = append(grid, k)
	}
	sort.Slice(grid, func(i, j int) bool {
		if grid[i].loss != grid[j].loss {
			return grid[i].loss < grid[j].loss
		}
		return grid[i].burst < grid[j].burst
	})

	var b strings.Builder
	b.WriteString("| loss% × burst | " + strings.Join(transports, " | ") + " |\n")
	b.WriteString("|---" + strings.Repeat("|---", len(transports)) + "|\n")
	for _, g := range grid {
		b.WriteString(fmt.Sprintf("| %g × %g |", g.loss, g.burst))
		for _, t := range transports {
			a, ok := aggs[BoundaryKey{Anchor: anchor, LoadLabel: loadLabel, LossPct: g.loss, BurstLen: g.burst, Transport: t}]
			b.WriteString(" " + formatBoundaryAgg(a, ok) + " |")
		}
		b.WriteString("\n")
	}
	b.WriteString("\n*セル = ブロック横断の staleness p99 中央値 [IQR] ms(n=有効ブロック数)。" +
		"`inv=k` = verdict が VALID でなく統計から除外したブロック数。全ブロックが invalid のときは `inv(n=k)`。" +
		"負荷 = " + loadLabel + "。*\n")
	return b.String()
}
