package aggregate

import (
	"encoding/json"
	"fmt"
	"math"
	"math/rand"
	"os"
	"path/filepath"
	"sort"
	"strings"

	"github.com/neguse/rudp-bench/orchestrator/sweep"
)

// bootstrap の既定パラメータ(capacity/boundary で共通)。
const (
	bootstrapIters   = 2000
	bootstrapConf    = 0.95
	minNForBootstrap = 3
)

// CapacityKey は 1 セル(transport × workload × regime)を識別する。
type CapacityKey struct {
	Transport string
	Workload  string
	Regime    string
}

// CapacityAgg はブロック横断で 1 セルを集約した結果。
//
// 集約則(censored/honest 混在の扱い — ここが本パッケージの核心的な設計判断):
//
//   - 各ブロックの capacity.json セルは「honest」(server 側の break を実際に
//     観測した実測値)か「censored」(Censored または RangeLimited — farm
//     律速 or 探索上限で打ち切り。値は「真の capacity ≥ v」という下限の
//     主張であって観測値ではない)のどちらか。
//
//   - censored blockを1件でも含む場合、それをexact observationとして捨てたり
//     下限値そのものとして扱ったりしない。各blockのexact/lower endpointから
//     sample medianの保守的下限を計算し、LowerBoundだけを報告する。
//
//   - 全blockがuncensoredの場合だけMedian/IQR/bootstrap CIを計算する。
//     first tested pointが失敗したleft-censored cellはinterval estimatorが未実装の
//     ため、誤った0を報告せず集約自体を拒否する。
type CapacityAgg struct {
	N            int   // このセルに寄与したブロック数(honest + censored)
	Values       []int // 各ブロックの生の capacity 値(dirs の順序どおり)
	CensoredN    int   // Values のうち censored/range-limited だった数
	HonestValues []int // Values のうち非 censored の値(統計の母集団)

	Median float64 // censored blockが無い場合だけ設定
	IQLo   float64 // HonestValues の 25 パーセンタイル
	IQHi   float64 // HonestValues の 75 パーセンタイル

	LowerBound    float64 // censored blockを含むsample medianの保守的下限
	MaxLowerBound float64 // legacy field; new aggregation leaves zero
	Conflicted    bool    // legacy field; new aggregation leaves false

	CILo, CIHi float64 // HonestValues の bootstrap median CI。len(HonestValues) < minNForBootstrap では 0
}

// AggregateCapacity は各ブロックの sweep 出力ディレクトリ(capacity.json を
// 持つ)を読み、(transport, workload, regime) セルごとに集約する。
// dirs の順序がそのまま各セルの Values / HonestValues の並び順になる。
func AggregateCapacity(dirs []string) (map[CapacityKey]CapacityAgg, error) {
	type raw struct {
		capacity int
		censored bool // Censored || RangeLimited(= 下限の主張)
	}
	byKey := map[CapacityKey][]raw{}
	comparisonByKey := map[CapacityKey]string{}
	campaignsByKey := map[CapacityKey]map[string]bool{}
	evidenceByKey := map[CapacityKey]map[string]string{}
	seenDirs := map[string]bool{}
	var order []CapacityKey
	hasLegacyIdentity := false
	hasComparisonIdentity := false

	for _, dir := range dirs {
		canonicalDir, err := filepath.Abs(filepath.Clean(dir))
		if err != nil {
			return nil, fmt.Errorf("resolve capacity input %s: %w", dir, err)
		}
		if seenDirs[canonicalDir] {
			return nil, fmt.Errorf("duplicate capacity input directory %s", dir)
		}
		seenDirs[canonicalDir] = true
		var doc struct {
			Cells []sweep.CellRecord `json:"cells"`
		}
		data, err := os.ReadFile(filepath.Join(dir, "capacity.json"))
		if err != nil {
			return nil, fmt.Errorf("%s: %w", dir, err)
		}
		if err := json.Unmarshal(data, &doc); err != nil {
			return nil, fmt.Errorf("%s/capacity.json: %w", dir, err)
		}
		seenInDir := map[CapacityKey]bool{}
		for _, c := range doc.Cells {
			key := CapacityKey{Transport: c.Transport, Workload: c.TestName(), Regime: c.Regime}
			if seenInDir[key] {
				return nil, fmt.Errorf("%s/capacity.json has duplicate cell %+v", dir, key)
			}
			seenInDir[key] = true
			if c.MeasurementInvalid {
				return nil, fmt.Errorf("capacity cell %+v is measurement_invalid; reacquisition is required before numeric aggregation", key)
			}
			if c.Outcome != "" {
				return nil, fmt.Errorf("capacity cell %+v has terminal outcome %s; numeric aggregation is not defined", key, c.Outcome)
			}
			if c.BelowRange {
				return nil, fmt.Errorf("capacity cell %+v is left-censored below conns=%d; interval aggregation is required", key, c.BreakConns)
			}
			if campaignsByKey[key] == nil {
				campaignsByKey[key] = map[string]bool{}
			}
			if c.CampaignIdentity != "" {
				if campaignsByKey[key][c.CampaignIdentity] {
					return nil, fmt.Errorf("capacity cell %+v repeats campaign_identity %q", key, c.CampaignIdentity)
				}
				campaignsByKey[key][c.CampaignIdentity] = true
			}
			if evidenceByKey[key] == nil {
				evidenceByKey[key] = map[string]string{}
			}
			for _, evidenceID := range c.EvidenceIDs {
				if previous, ok := evidenceByKey[key][evidenceID]; ok {
					return nil, fmt.Errorf("capacity cell %+v reuses acquisition %q in %s and %s", key, evidenceID, previous, dir)
				}
				evidenceByKey[key][evidenceID] = dir
			}
			if c.ComparisonIdentity == "" {
				hasLegacyIdentity = true
			} else {
				hasComparisonIdentity = true
			}
			if comparison, ok := comparisonByKey[key]; ok && comparison != c.ComparisonIdentity {
				return nil, fmt.Errorf("capacity cell %+v comparison identity mismatch: %q != %q", key, comparison, c.ComparisonIdentity)
			}
			comparisonByKey[key] = c.ComparisonIdentity
			if _, ok := byKey[key]; !ok {
				order = append(order, key)
			}
			byKey[key] = append(byKey[key], raw{capacity: c.Capacity, censored: c.Censored || c.RangeLimited})
		}
	}
	if hasLegacyIdentity && hasComparisonIdentity {
		return nil, fmt.Errorf("cannot mix legacy capacity cells without comparison_identity with identified cells")
	}
	if hasLegacyIdentity && len(dirs) > 1 {
		return nil, fmt.Errorf("cannot aggregate multiple legacy capacity inputs without comparison_identity")
	}

	out := make(map[CapacityKey]CapacityAgg, len(byKey))
	// CI 計算専用の rng。固定シードなので同じ入力からは常に同じ結果になる
	// (AggregateCapacity は乱数源を受け取らないため、内部で固定する)。
	rng := rand.New(rand.NewSource(1))
	for _, key := range order {
		rows := byKey[key]
		agg := CapacityAgg{N: len(rows)}
		var honestF, lowerBounds []float64
		for _, r := range rows {
			agg.Values = append(agg.Values, r.capacity)
			if r.censored {
				agg.CensoredN++
			} else {
				agg.HonestValues = append(agg.HonestValues, r.capacity)
				honestF = append(honestF, float64(r.capacity))
			}
			lowerBounds = append(lowerBounds, float64(r.capacity))
		}
		sort.Float64s(honestF)
		sort.Float64s(lowerBounds)

		switch {
		case agg.CensoredN > 0:
			// Right-censored blockをexact observationとして扱わない。各値を
			// lower endpointとしたsample medianの保守的下限だけを報告する。
			agg.LowerBound = median(lowerBounds)
		case len(honestF) > 0:
			agg.Median = median(honestF)
			agg.IQLo = percentile(honestF, 0.25)
			agg.IQHi = percentile(honestF, 0.75)
			if len(honestF) >= minNForBootstrap {
				agg.CILo, agg.CIHi = BootstrapMedianCI(honestF, bootstrapIters, bootstrapConf, rng)
			}
		}
		out[key] = agg
	}
	return out, nil
}

// transportOrder は report/report.go の表列順を写した複製。
// report パッケージを import しない方針のため複製している — 列順を変える
// 場合は両方を同期すること。
var transportOrder = []string{"enet", "gns", "litenetlib", "msquic", "websocket", "magiconion"}

// workloadRows は report/report.go の行順(workloadRows)を写した複製。
var workloadRows = []struct {
	Name   string
	Anchor string
}{
	{"r10p128", ""}, {"r10p200", ""}, {"r10p1000", ""},
	{"r20p128", "br"}, {"r20p200", ""}, {"r20p1000", "video"},
	{"r60p128", ""}, {"r60p200", "vr"}, {"r60p1000", ""},
	{"echo", "synthetic"}, {"reliable_echo", "synthetic"},
}

// formatNum は整数値なら小数点なし、そうでなければ小数第1位までで表示する。
func formatNum(v float64) string {
	if v == math.Trunc(v) {
		return fmt.Sprintf("%.0f", v)
	}
	return fmt.Sprintf("%.1f", v)
}

func formatCapacityAgg(a CapacityAgg, found bool) string {
	if !found {
		return "—"
	}
	value, lo, hi := a.Median, a.IQLo, a.IQHi
	prefix := ""
	if a.CensoredN > 0 {
		// 下限のみ: honest 実測が無いので IQR は意味を持たず、値そのものを
		// 3箇所に置いて縮退させる。
		prefix = "≥"
		value, lo, hi = a.LowerBound, a.LowerBound, a.LowerBound
	}
	suffix := ""
	if a.CensoredN > 0 && a.CensoredN < a.N {
		suffix = fmt.Sprintf(" (right-censored %d/%d)", a.CensoredN, a.N)
	}
	return fmt.Sprintf("%s%s [%s–%s] (n=%d)%s", prefix, formatNum(value), formatNum(lo), formatNum(hi), a.N, suffix)
}

// CapacityCITable は workload 行 × transport 列の markdown 表(ブロック集約
// 版)を生成する。onlyAnchors=true で anchor 3 セルに絞る(report.go の
// CapacityTable と同じ絞り込み規則)。
func CapacityCITable(aggs map[CapacityKey]CapacityAgg, regime string, onlyAnchors bool) string {
	present := map[string]bool{}
	for k := range aggs {
		if k.Regime == regime {
			present[k.Transport] = true
		}
	}
	var transports []string
	seenTransport := map[string]bool{}
	for _, t := range transportOrder {
		if present[t] {
			transports = append(transports, t)
			seenTransport[t] = true
		}
	}
	var extraTransports []string
	for name := range present {
		if !seenTransport[name] {
			extraTransports = append(extraTransports, name)
		}
	}
	sort.Strings(extraTransports)
	transports = append(transports, extraTransports...)

	known := map[string]bool{}
	for _, row := range workloadRows {
		known[row.Name] = true
	}
	var scenarioRows []string
	if !onlyAnchors {
		seen := map[string]bool{}
		for key := range aggs {
			if key.Regime == regime && !known[key.Workload] && !seen[key.Workload] {
				scenarioRows = append(scenarioRows, key.Workload)
				seen[key.Workload] = true
			}
		}
		sort.Strings(scenarioRows)
	}

	var b strings.Builder
	rowHeader := "workload"
	if len(scenarioRows) > 0 {
		rowHeader = "scenario / workload"
	}
	b.WriteString("| " + rowHeader + " | " + strings.Join(transports, " | ") + " |\n")
	b.WriteString("|---" + strings.Repeat("|---", len(transports)) + "|\n")
	for _, row := range workloadRows {
		isAnchor := row.Anchor != "" && row.Anchor != "synthetic"
		if onlyAnchors && !isAnchor {
			continue
		}
		label := row.Name
		if isAnchor {
			label += " ⚓" + row.Anchor
		} else if row.Anchor == "synthetic" {
			label += " (synthetic)"
		}
		b.WriteString("| " + label + " |")
		for _, t := range transports {
			a, ok := aggs[CapacityKey{Transport: t, Workload: row.Name, Regime: regime}]
			b.WriteString(" " + formatCapacityAgg(a, ok) + " |")
		}
		b.WriteString("\n")
	}
	for _, name := range scenarioRows {
		b.WriteString("| " + name + " |")
		for _, t := range transports {
			a, ok := aggs[CapacityKey{Transport: t, Workload: name, Regime: regime}]
			b.WriteString(" " + formatCapacityAgg(a, ok) + " |")
		}
		b.WriteString("\n")
	}
	b.WriteString("\n*凡例: `median [iqlo–iqhi] (n=N)` = uncensored blockのcapacity中央値とIQR。" +
		"censored blockを1件でも含む場合は、right-censored sample medianの保守的下限`≥N`だけを表示する。*\n")
	return b.String()
}
