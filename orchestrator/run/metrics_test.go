package run

import (
	"encoding/json"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"testing"
)

func TestMergeMetricsGolden(t *testing.T) {
	dir := t.TempDir()
	paths := []string{
		writeMetricsFixture(t, dir, "proc0.json", metricsFixtureString(
			ClassCounts{Slots: 10, SlotsBroadcast: 2, Submitted: 9, DeliveredUnique: 12, Duplicates: 1},
			ClassCounts{Slots: 4, Submitted: 4, DeliveredUnique: 4, DeadlineHit: 3},
			bins(map[int]uint64{0: 1, 16: 2}),
			bins(map[int]uint64{1: 3}),
			bins(map[int]uint64{2: 4}),
			bins(map[int]uint64{3: 4}),
			bins(map[int]uint64{0: 1, 20: 1}),
			RawCounts{Slots: 14, Submitted: 13, RecvMeasured: 16, RecvUnmeasured: 2},
		)),
		writeMetricsFixture(t, dir, "proc1.json", metricsFixtureString(
			ClassCounts{Slots: 5, SlotsBroadcast: 1, Submitted: 5, DeliveredUnique: 6, Duplicates: 2},
			ClassCounts{Slots: 6, Submitted: 6, DeliveredUnique: 6, DeadlineHit: 5},
			bins(map[int]uint64{31: 1}),
			bins(map[int]uint64{4: 1}),
			bins(map[int]uint64{16: 6}),
			bins(map[int]uint64{17: 6}),
			bins(map[int]uint64{31: 1}),
			RawCounts{Slots: 11, Submitted: 11, RecvMeasured: 12, RecvUnmeasured: 1},
		)),
	}

	merged, err := MergeMetricsFiles(paths, 3)
	if err != nil {
		t.Fatal(err)
	}

	loss := merged.Classes[ClassLossTolerant]
	if loss.Slots != 15 || loss.SlotsBroadcast != 3 || loss.Submitted != 14 || loss.DeliveredUnique != 18 || loss.Duplicates != 3 {
		t.Fatalf("loss counts = %+v", loss.ClassCounts)
	}
	if loss.ExpectedReceives != 21 {
		t.Fatalf("loss expected_receives = %d, want 21", loss.ExpectedReceives)
	}
	assertFloat(t, "loss delivery", loss.DeliveryRatio, float64(18)/21)
	assertFloat(t, "loss attempted", loss.AttemptedRatio, float64(14)/15)
	if loss.LatencySchedNS.Count != 4 || loss.LatencySchedNS.P50NS != 2125 || loss.LatencySchedNS.P90NS != 100000000000 {
		t.Fatalf("loss latency sched hist = count %d p50 %d p90 %d", loss.LatencySchedNS.Count, loss.LatencySchedNS.P50NS, loss.LatencySchedNS.P90NS)
	}

	must := merged.Classes[ClassMustDeliver]
	if must.Slots != 10 || must.Submitted != 10 || must.DeliveredUnique != 10 || must.DeadlineHit != 8 {
		t.Fatalf("must counts = %+v", must.ClassCounts)
	}
	if must.ExpectedReceives != 10 {
		t.Fatalf("must expected_receives = %d, want 10", must.ExpectedReceives)
	}
	assertFloat(t, "must delivery", must.DeliveryRatio, 1)
	assertFloat(t, "must deadline", must.DeadlineHitRatio, 0.8)

	if merged.StalenessNS.Count != 3 || merged.StalenessNS.P50NS != 2625 || merged.StalenessNS.P90NS != 100000000000 {
		t.Fatalf("staleness hist = count %d p50 %d p90 %d", merged.StalenessNS.Count, merged.StalenessNS.P50NS, merged.StalenessNS.P90NS)
	}
	if merged.Raw != (RawCounts{Slots: 25, Submitted: 24, RecvMeasured: 28, RecvUnmeasured: 3}) {
		t.Fatalf("raw = %+v", merged.Raw)
	}
}

func writeMetricsFixture(t *testing.T, dir, name, data string) string {
	t.Helper()
	path := filepath.Join(dir, name)
	if err := os.WriteFile(path, []byte(data), 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

func metricsFixtureString(loss, must ClassCounts, lossSched, lossSend, mustSched, mustSend, staleness []uint64, raw RawCounts) string {
	return fmt.Sprintf(`{
  "version": 1,
  "histogram": {"scheme":"log2x16","subbins":16,"min_ns":1000,"max_ns":100000000000},
  "classes": {
    "loss_tolerant": %s,
    "must_deliver": %s
  },
  "staleness_ns": %s,
  "raw": {"slots":%d,"submitted":%d,"recv_measured":%d,"recv_unmeasured":%d}
}`,
		classFixtureString(loss, lossSched, lossSend),
		classFixtureString(must, mustSched, mustSend),
		histFixtureString(staleness),
		raw.Slots, raw.Submitted, raw.RecvMeasured, raw.RecvUnmeasured)
}

func classFixtureString(counts ClassCounts, sched, send []uint64) string {
	return fmt.Sprintf(`{"slots":%d,"slots_broadcast":%d,"submitted":%d,"delivered_unique":%d,"duplicates":%d,"deadline_hit":%d,"latency_sched_ns":%s,"latency_send_ns":%s}`,
		counts.Slots,
		counts.SlotsBroadcast,
		counts.Submitted,
		counts.DeliveredUnique,
		counts.Duplicates,
		counts.DeadlineHit,
		histFixtureString(sched),
		histFixtureString(send))
}

func histFixtureString(b []uint64) string {
	return fmt.Sprintf(`{"scheme":"log2x16","min_ns":1000,"max_ns":100000000000,"count":%d,"p50_ns":0,"p90_ns":0,"p99_ns":0,"bins":%s}`,
		sumBins(b), binsLiteral(b))
}

func bins(counts map[int]uint64) []uint64 {
	out := make([]uint64, 32)
	for i, v := range counts {
		out[i] = v
	}
	return out
}

func binsLiteral(b []uint64) string {
	data, err := json.Marshal(b)
	if err != nil {
		panic(err)
	}
	return string(data)
}

func sumBins(b []uint64) uint64 {
	var out uint64
	for _, v := range b {
		out += v
	}
	return out
}

func assertFloat(t *testing.T, name string, got, want float64) {
	t.Helper()
	if math.Abs(got-want) > 0.0000001 {
		t.Fatalf("%s = %g, want %g", name, got, want)
	}
}
