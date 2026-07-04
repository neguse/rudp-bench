package aggregate

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/neguse/rudp-bench/orchestrator/boundary"
	"github.com/neguse/rudp-bench/orchestrator/sweep"
)

// writeBoundaryBlock は 1 ブロック分の results.jsonl を t.TempDir() 配下に書く。
func writeBoundaryBlock(t *testing.T, recs []boundary.PointRecord) string {
	t.Helper()
	dir := t.TempDir()
	f, err := os.Create(filepath.Join(dir, "results.jsonl"))
	if err != nil {
		t.Fatal(err)
	}
	defer f.Close()
	for _, r := range recs {
		line, err := json.Marshal(r)
		if err != nil {
			t.Fatal(err)
		}
		if _, err := f.Write(append(line, '\n')); err != nil {
			t.Fatal(err)
		}
	}
	return dir
}

func boundaryPoint(transport, anchor string, loss, burst float64, label string, conns int, verdict string, p99NS uint64) boundary.PointRecord {
	return boundary.PointRecord{
		Transport: transport,
		Anchor:    anchor,
		LossPct:   loss,
		BurstLen:  burst,
		Load:      boundary.Load{Label: label, Conns: conns},
		Verdict:   verdict,
		Judgment:  sweep.Judgment{StalenessP99: p99NS},
	}
}

// 1 ブロックが INVALID: 統計から除外されるが InvalidN でカウントされる。
func TestAggregateBoundaryExcludesInvalidButCounts(t *testing.T) {
	dirs := []string{
		writeBoundaryBlock(t, []boundary.PointRecord{
			boundaryPoint("enet", "r20p128", 1.0, 4.0, "floor", 4, "VALID", 100_000_000),
		}),
		writeBoundaryBlock(t, []boundary.PointRecord{
			boundaryPoint("enet", "r20p128", 1.0, 4.0, "floor", 4, "VALID", 120_000_000),
		}),
		writeBoundaryBlock(t, []boundary.PointRecord{
			boundaryPoint("enet", "r20p128", 1.0, 4.0, "floor", 4, "INVALID", 999_000_000),
		}),
	}
	aggs, err := AggregateBoundary(dirs)
	if err != nil {
		t.Fatal(err)
	}
	key := BoundaryKey{Anchor: "r20p128", LoadLabel: "floor", LossPct: 1.0, BurstLen: 4.0, Transport: "enet"}
	a, ok := aggs[key]
	if !ok {
		t.Fatal("cell not found")
	}
	if a.N != 2 {
		t.Fatalf("N=%d, want 2 (VALID only)", a.N)
	}
	if a.InvalidN != 1 {
		t.Fatalf("InvalidN=%d, want 1", a.InvalidN)
	}
	wantMedianMS := 110.0 // (100+120)/2
	if a.MedianMS != wantMedianMS {
		t.Fatalf("MedianMS=%v, want %v", a.MedianMS, wantMedianMS)
	}
	// invalid ブロックの p99(999ms)が統計に混ざっていないことを確認
	if a.IQHiMS >= 999 {
		t.Fatalf("invalid block's p99 leaked into stats: IQHiMS=%v", a.IQHiMS)
	}
}

func TestAggregateBoundaryAllInvalid(t *testing.T) {
	dirs := []string{
		writeBoundaryBlock(t, []boundary.PointRecord{
			boundaryPoint("gns", "r60p200", 5.0, 8.0, "q50", 10, "INVALID", 0),
		}),
		writeBoundaryBlock(t, []boundary.PointRecord{
			boundaryPoint("gns", "r60p200", 5.0, 8.0, "q50", 10, "INVALID", 0),
		}),
	}
	aggs, err := AggregateBoundary(dirs)
	if err != nil {
		t.Fatal(err)
	}
	key := BoundaryKey{Anchor: "r60p200", LoadLabel: "q50", LossPct: 5.0, BurstLen: 8.0, Transport: "gns"}
	a := aggs[key]
	if a.N != 0 || a.InvalidN != 2 {
		t.Fatalf("N=%d InvalidN=%d, want 0/2", a.N, a.InvalidN)
	}
	table := BoundaryCITable(aggs, "r60p200", "q50")
	if !strings.Contains(table, "inv(n=2)") {
		t.Fatalf("table missing all-invalid marker:\n%s", table)
	}
}

func TestAggregateBoundarySingleBlockNoCI(t *testing.T) {
	dirs := []string{
		writeBoundaryBlock(t, []boundary.PointRecord{
			boundaryPoint("litenetlib", "r20p1000", 1.0, 2.0, "floor", 4, "VALID", 50_000_000),
		}),
	}
	aggs, err := AggregateBoundary(dirs)
	if err != nil {
		t.Fatal(err)
	}
	key := BoundaryKey{Anchor: "r20p1000", LoadLabel: "floor", LossPct: 1.0, BurstLen: 2.0, Transport: "litenetlib"}
	a := aggs[key]
	if a.N != 1 || a.MedianMS != 50 || a.IQLoMS != 50 || a.IQHiMS != 50 {
		t.Fatalf("unexpected single-block stats: %+v", a)
	}
	if a.CILoMS != 0 || a.CIHiMS != 0 {
		t.Fatalf("N=1 should not produce a CI, got %v/%v", a.CILoMS, a.CIHiMS)
	}
}
