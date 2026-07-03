package sweep

import "fmt"

// PointOutcome は探索が必要とする 1 点の評価結果。
type PointOutcome struct {
	OK       bool
	Censored bool
	Cause    string
}

// CellCapacity は 1 セル(transport × workload × regime)の探索結果。
type CellCapacity struct {
	Capacity int `json:"capacity"` // 最後に OK だった conns(0 = min で break)
	// Censored: capacity は下限値(farm 律速で打ち切り。server の break ではない)
	Censored bool `json:"censored,omitempty"`
	// RangeLimited: 探索上限まで OK(capacity ≥ max)
	RangeLimited bool   `json:"range_limited,omitempty"`
	BreakConns   int    `json:"break_conns,omitempty"` // 最初の非 OK 点(break 時のみ)
	BreakCause   string `json:"break_cause,omitempty"`
	Evaluated    int    `json:"evaluated_points"`
}

// FindCapacity は指数増加 + 二分探索で quality-bounded capacity を求める。
// eval は同一 conns に対して安定な結果を返すこと(resume cache 前提)。
func FindCapacity(eval func(conns int) (PointOutcome, error), min, max int) (CellCapacity, error) {
	if min < 1 || max < min {
		return CellCapacity{}, fmt.Errorf("invalid conns range [%d, %d]", min, max)
	}
	cell := CellCapacity{}
	call := func(conns int) (PointOutcome, error) {
		cell.Evaluated++
		return eval(conns)
	}

	out, err := call(min)
	if err != nil {
		return cell, err
	}
	if out.Censored {
		cell.Censored = true
		cell.Capacity = 0
		cell.BreakCause = out.Cause
		return cell, nil
	}
	if !out.OK {
		cell.Capacity = 0
		cell.BreakConns = min
		cell.BreakCause = out.Cause
		return cell, nil
	}

	// 指数増加で上界を掴む
	lastOK := min
	firstFail := 0
	failCause := ""
	for c := min * 2; ; c *= 2 {
		if c > max {
			c = max
		}
		if c == lastOK { // min == max
			break
		}
		out, err := call(c)
		if err != nil {
			return cell, err
		}
		if out.Censored {
			cell.Censored = true
			cell.Capacity = lastOK
			cell.BreakCause = out.Cause
			return cell, nil
		}
		if out.OK {
			lastOK = c
			if c == max {
				break
			}
			continue
		}
		firstFail = c
		failCause = out.Cause
		break
	}
	if firstFail == 0 {
		cell.Capacity = lastOK
		cell.RangeLimited = lastOK == max
		return cell, nil
	}

	// (lastOK, firstFail) を二分
	lo, hi := lastOK, firstFail
	for hi-lo > 1 {
		mid := lo + (hi-lo)/2
		out, err := call(mid)
		if err != nil {
			return cell, err
		}
		if out.Censored {
			cell.Censored = true
			cell.Capacity = lo
			cell.BreakCause = out.Cause
			return cell, nil
		}
		if out.OK {
			lo = mid
		} else {
			hi = mid
			failCause = out.Cause
		}
	}
	cell.Capacity = lo
	cell.BreakConns = hi
	cell.BreakCause = failCause
	return cell, nil
}
