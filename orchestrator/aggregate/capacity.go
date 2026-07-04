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
//   - 全ブロックが censored(CensoredN == N)の場合、集約全体が下限の主張に
//     なる。各ブロックは capacity_i ≥ v_i を主張しているので、これらを
//     まとめた最もタイトな(=最も強い)正しい主張は capacity ≥ max(v_i)。
//     これを LowerBound に入れ、Median/IQLo/IQHi は計算しない(実測が無い)。
//
//   - honest と censored が混在する場合は honest 値が実測なので、それを
//     母集団として Median/IQLo/IQHi/CI を計算する(HonestValues 上で計算)。
//     censored 側の下限値は「honest の最大値以下」なら honest 実測に
//     既に上回られているので無視してよい。逆に censored の下限値が honest
//     の最大値を上回るなら「本当はもっと高いかもしれない」という食い違いの
//     シグナルなので握りつぶさず、MaxLowerBound に記録し Conflicted=true と
//     する(Median 等は honest ベースのまま報告し、注記として併記する)。
type CapacityAgg struct {
	N            int   // このセルに寄与したブロック数(honest + censored)
	Values       []int // 各ブロックの生の capacity 値(dirs の順序どおり)
	CensoredN    int   // Values のうち censored/range-limited だった数
	HonestValues []int // Values のうち非 censored の値(統計の母集団)

	Median float64 // HonestValues の中央値。HonestValues が空なら 0(LowerBound を見る)
	IQLo   float64 // HonestValues の 25 パーセンタイル
	IQHi   float64 // HonestValues の 75 パーセンタイル

	LowerBound    float64 // CensoredN == N のときのみ設定: max(Values)
	MaxLowerBound float64 // 混在かつ censored の最大値が honest の最大値を超えるときのみ設定
	Conflicted    bool    // MaxLowerBound が設定されている(honest と censored の食い違いあり)

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
	var order []CapacityKey

	for _, dir := range dirs {
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
		for _, c := range doc.Cells {
			key := CapacityKey{Transport: c.Transport, Workload: c.Workload, Regime: c.Regime}
			if _, ok := byKey[key]; !ok {
				order = append(order, key)
			}
			byKey[key] = append(byKey[key], raw{capacity: c.Capacity, censored: c.Censored || c.RangeLimited})
		}
	}

	out := make(map[CapacityKey]CapacityAgg, len(byKey))
	// CI 計算専用の rng。固定シードなので同じ入力からは常に同じ結果になる
	// (AggregateCapacity は乱数源を受け取らないため、内部で固定する)。
	rng := rand.New(rand.NewSource(1))
	for _, key := range order {
		rows := byKey[key]
		agg := CapacityAgg{N: len(rows)}
		var honestF []float64
		maxCensored := 0
		hasCensored := false
		for _, r := range rows {
			agg.Values = append(agg.Values, r.capacity)
			if r.censored {
				agg.CensoredN++
				if !hasCensored || r.capacity > maxCensored {
					maxCensored = r.capacity
				}
				hasCensored = true
			} else {
				agg.HonestValues = append(agg.HonestValues, r.capacity)
				honestF = append(honestF, float64(r.capacity))
			}
		}
		sort.Float64s(honestF)

		switch {
		case agg.CensoredN == agg.N:
			// 全 censored → 下限の主張のみ(honest 実測なし)。
			agg.LowerBound = float64(maxCensored)
		case len(honestF) > 0:
			agg.Median = median(honestF)
			agg.IQLo = percentile(honestF, 0.25)
			agg.IQHi = percentile(honestF, 0.75)
			if hasCensored {
				honestMax := honestF[len(honestF)-1]
				if float64(maxCensored) > honestMax {
					agg.MaxLowerBound = float64(maxCensored)
					agg.Conflicted = true
				}
				// maxCensored <= honestMax の場合は honest 実測に既に
				// 上回られているので無視する(意図的に何もしない)。
			}
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
	if a.CensoredN == a.N {
		// 下限のみ: honest 実測が無いので IQR は意味を持たず、値そのものを
		// 3箇所に置いて縮退させる。
		prefix = "≥"
		value, lo, hi = a.LowerBound, a.LowerBound, a.LowerBound
	}
	suffix := ""
	if a.Conflicted {
		suffix = "!"
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
	for _, t := range transportOrder {
		if present[t] {
			transports = append(transports, t)
		}
	}

	var b strings.Builder
	b.WriteString("| workload | " + strings.Join(transports, " | ") + " |\n")
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
	b.WriteString("\n*凡例: `median [iqlo–iqhi] (n=N)` = ブロック横断の honest capacity 中央値と IQR。" +
		"`≥N (n=N)`(下限のみ)= 全ブロックが打ち切りで、N は max の下限値。" +
		"末尾 `!` = 一部ブロックの censored 下限が honest 最大値を上回る食い違いあり。*\n")
	return b.String()
}
