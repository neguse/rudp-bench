package aggregate

import (
	"math/rand"
	"sort"
)

// BootstrapMedianCI は中央値の percentile bootstrap 信頼区間を計算する。
// values から復元抽出で同サイズの標本を iters 回作り、各標本の中央値を
// 集めて分布を作る。conf(例: 0.95)に対する下側/上側 (1-conf)/2 分位点を
// (lo, hi) として返す。rng を固定すれば呼び出しごとに決定的(同じ values/
// iters/conf/rng シードなら常に同じ結果)。
//
// values が空、または iters <= 0 の場合は (0, 0) を返す(呼び出し側は
// N が十分な場合のみ呼ぶ想定 — 本パッケージでは N>=3 をその閾値としている)。
func BootstrapMedianCI(values []float64, iters int, conf float64, rng *rand.Rand) (lo, hi float64) {
	n := len(values)
	if n == 0 || iters <= 0 {
		return 0, 0
	}
	medians := make([]float64, iters)
	resample := make([]float64, n)
	for i := 0; i < iters; i++ {
		for j := 0; j < n; j++ {
			resample[j] = values[rng.Intn(n)]
		}
		sorted := append([]float64(nil), resample...)
		sort.Float64s(sorted)
		medians[i] = median(sorted)
	}
	sort.Float64s(medians)
	alpha := 1 - conf
	lo = percentile(medians, alpha/2)
	hi = percentile(medians, 1-alpha/2)
	return lo, hi
}
