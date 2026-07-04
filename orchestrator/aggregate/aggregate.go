// Package aggregate は E3(ブロック反復)の横断集約を行う。
// 1 ブロック = 1 host session の capacity sweep / boundary 出力(既存の
// orchestrator/sweep, orchestrator/boundary が書く capacity.json /
// results.jsonl)を複数読み、セルごとに中央値・IQR・percentile bootstrap CI
// にまとめ、report/report.go と同じ見た目の markdown 表を出す。
//
// CLI 配線(orchestrator aggregate サブコマンド)は別タスクで行うため、
// このパッケージは orchestrator/cmd 等の既存ファイルに一切手を入れない。
package aggregate

import "sort"

// percentile は昇順ソート済み sortedValues の p 分位点を線形補間で返す
// (numpy の 'linear' method と同じ定義)。0 <= p <= 1。
// sortedValues が空なら 0。要素が 1 つなら常にその値(IQR/CI が縮退して
// 1点に一致する = N=1 セルの「IQR なし」を自然に表現する)。
func percentile(sortedValues []float64, p float64) float64 {
	n := len(sortedValues)
	if n == 0 {
		return 0
	}
	if n == 1 {
		return sortedValues[0]
	}
	idx := p * float64(n-1)
	lo := int(idx)
	hi := lo + 1
	if hi >= n {
		return sortedValues[n-1]
	}
	frac := idx - float64(lo)
	return sortedValues[lo]*(1-frac) + sortedValues[hi]*frac
}

// median は percentile(values, 0.5) の別名(可読性のため)。
func median(sortedValues []float64) float64 { return percentile(sortedValues, 0.5) }

// sortedCopy は values を破壊せずソート済みコピーを返す。
func sortedCopy(values []float64) []float64 {
	out := append([]float64(nil), values...)
	sort.Float64s(out)
	return out
}
