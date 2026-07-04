package sweep

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"

	"github.com/neguse/rudp-bench/orchestrator/run"
)

// Rejudge は既存 sweep 出力の判定だけを run 成果物(result.json)から
// 再計算する。再測定はしない — gate/floor の意味論を更新したときに、
// 測定済みデータへ新しい判定を適用するために使う。
// capacity.json は探索済みの点集合から再導出する(単調性を仮定)。
func Rejudge(dir string, schedMeasurand map[string]bool) error {
	resultsPath := filepath.Join(dir, "results.jsonl")
	f, err := os.Open(resultsPath)
	if err != nil {
		return err
	}
	var records []PointRecord
	sc := bufio.NewScanner(f)
	sc.Buffer(make([]byte, 1<<20), 1<<20)
	for sc.Scan() {
		var rec PointRecord
		if json.Unmarshal(sc.Bytes(), &rec) != nil {
			continue
		}
		records = append(records, rec)
	}
	if err := sc.Err(); err != nil {
		f.Close()
		return err
	}
	f.Close()

	for i := range records {
		rec := &records[i]
		data, err := os.ReadFile(filepath.Join(rec.RunDir, "result.json"))
		if err != nil {
			return fmt.Errorf("%s: %w", rec.RunDir, err)
		}
		var res run.Result
		if err := json.Unmarshal(data, &res); err != nil {
			return fmt.Errorf("%s/result.json: %w", rec.RunDir, err)
		}
		w, ok := run.LookupWorkload(res.Config.Workload)
		if !ok {
			return fmt.Errorf("%s: unknown workload %q", rec.RunDir, res.Config.Workload)
		}
		// 過去 run の result.json にはフラグが無いため、transport 単位で上書きする
		if schedMeasurand[rec.Transport] {
			res.Config.SchedIsMeasurand = true
		}
		old := rec.Judgment
		rec.Judgment = Judge(&res, w, res.Config.TotalConns, res.Config.Netem, res.Config.StalenessPeriodNS)
		if old.OK != rec.Judgment.OK || old.Censored != rec.Judgment.Censored {
			fmt.Fprintf(os.Stderr, "[rejudge] %s|%s c%d: ok=%v→%v censored=%v→%v %s\n",
				rec.Transport, rec.Workload, rec.Conns, old.OK, rec.Judgment.OK, old.Censored, rec.Judgment.Censored, rec.Judgment.Cause)
		}
	}

	// results.jsonl を書き直す
	tmp := resultsPath + ".tmp"
	out, err := os.Create(tmp)
	if err != nil {
		return err
	}
	for _, rec := range records {
		line, err := json.Marshal(rec)
		if err != nil {
			out.Close()
			return err
		}
		if _, err := out.Write(append(line, '\n')); err != nil {
			out.Close()
			return err
		}
	}
	if err := out.Close(); err != nil {
		return err
	}
	if err := os.Rename(tmp, resultsPath); err != nil {
		return err
	}

	// capacity.json をセルごとに再導出
	byCell := map[string][]PointRecord{}
	for _, rec := range records {
		key := rec.Transport + "|" + rec.Workload + "|" + rec.Regime
		byCell[key] = append(byCell[key], rec)
	}
	keys := make([]string, 0, len(byCell))
	for k := range byCell {
		keys = append(keys, k)
	}
	sort.Strings(keys)

	seed := readSeed(filepath.Join(dir, "capacity.json"))
	var cells []CellRecord
	for _, k := range keys {
		points := byCell[k]
		rec := CellRecord{
			Transport:    points[0].Transport,
			Workload:     points[0].Workload,
			Regime:       points[0].Regime,
			CellCapacity: deriveCell(points),
		}
		cells = append(cells, rec)
	}
	data, err := json.MarshalIndent(struct {
		Seed  int64        `json:"seed"`
		Cells []CellRecord `json:"cells"`
	}{seed, cells}, "", " ")
	if err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(dir, "capacity.json"), append(data, '\n'), 0o644)
}

func readSeed(capacityPath string) int64 {
	var doc struct {
		Seed int64 `json:"seed"`
	}
	data, err := os.ReadFile(capacityPath)
	if err == nil {
		_ = json.Unmarshal(data, &doc)
	}
	return doc.Seed
}

// deriveCell は探索済み点集合から capacity 結論を再導出する(単調性仮定)。
// 探索順は再現しないため、bisection が本来引いた境界の近似になる。
func deriveCell(points []PointRecord) CellCapacity {
	sort.Slice(points, func(i, j int) bool { return points[i].Conns < points[j].Conns })
	cell := CellCapacity{Evaluated: len(points)}
	for _, p := range points {
		if p.Judgment.OK {
			cell.Capacity = p.Conns
		}
	}
	firstFail, firstCensor := 0, 0
	var failCause, censorCause string
	for _, p := range points {
		if p.Conns <= cell.Capacity {
			continue
		}
		if p.Judgment.Censored {
			if firstCensor == 0 {
				firstCensor = p.Conns
				censorCause = p.Judgment.Cause
			}
			continue
		}
		if !p.Judgment.OK && firstFail == 0 {
			firstFail = p.Conns
			failCause = p.Judgment.Cause
		}
	}
	switch {
	case firstCensor > 0 && (firstFail == 0 || firstCensor < firstFail):
		cell.Censored = true
		cell.BreakCause = censorCause
	case firstFail > 0:
		cell.BreakConns = firstFail
		cell.BreakCause = failCause
	default:
		cell.RangeLimited = true
	}
	return cell
}
