package run

import (
	"encoding/json"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"strings"
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

func TestMergeMetricsTrafficSeries(t *testing.T) {
	dir := t.TempDir()
	h := histFixtureString(v2Bins(map[int]uint64{4: 1}))
	lossClass := v2ClassFixtureString(
		ClassCounts{Slots: 3, Submitted: 3, DeliveredUnique: 3, ExpectedFlows: 1, ObservedFlows: 1},
		v2Bins(map[int]uint64{4: 1}), v2Bins(map[int]uint64{4: 1}), v2Bins(map[int]uint64{4: 1}))
	emptyClass := v2ClassFixtureString(ClassCounts{}, v2Bins(nil), v2Bins(nil), v2Bins(nil))
	file := fmt.Sprintf(`{
  "version": 2,
  "histogram": {"scheme":"log2x16","subbins":16,"min_ns":1000,"max_ns":100000000000},
  "classes": {"loss_tolerant": %s, "must_deliver": %s},
  "staleness_ns": %s,
	"update_gap_ns": %s,
  "raw": {"slots":3,"submitted":3,"recv_measured":3,"recv_unmeasured":0},
  "traffic": [
    {"traffic_id":2,"direction":"server_to_client","class":"loss_tolerant","deadline_ns":0,
     "slots":3,"slots_broadcast":0,"submitted":3,"delivered_unique":3,"duplicates":0,"deadline_hit":0,
	 "expected_flows":1,"observed_flows":1,"never_received_flows":0,
     "latency_sched_ns":%s,"latency_send_ns":%s,"update_gap_ns":%s,"staleness_ns":%s}
  ]
	}`, lossClass, emptyClass, h, h, h, h, h, h)
	path := writeMetricsFixture(t, dir, "traffic.json", file)

	merged, err := MergeMetricsFiles([]string{path}, 3)
	if err != nil {
		t.Fatal(err)
	}
	if merged.Version != 2 {
		t.Fatalf("merged version = %d, want 2", merged.Version)
	}
	metric, ok := merged.TrafficMetric(2, DirectionServerToClient, ClassLossTolerant)
	if !ok {
		t.Fatal("server state traffic series missing")
	}
	if metric.ExpectedReceives != 3 || metric.DeliveryRatio != 1 || metric.StalenessNS.Count != 1 {
		t.Fatalf("traffic metric = %+v", metric)
	}
}

func TestMergeMetricsRejectsLegacyTrafficAggregateMismatch(t *testing.T) {
	dir := t.TempDir()
	emptyBins := v2Bins(nil)
	oneBin := v2Bins(map[int]uint64{4: 1})
	emptyHist := histFixtureString(emptyBins)
	oneHist := histFixtureString(oneBin)
	emptyClass := v2ClassFixtureString(ClassCounts{}, emptyBins, emptyBins, emptyBins)
	mustClass := v2ClassFixtureString(
		ClassCounts{Slots: 1, Submitted: 1, DeliveredUnique: 1, DeadlineHit: 0},
		oneBin, oneBin, emptyBins)
	file := fmt.Sprintf(`{
  "version": 2,
  "histogram": {"scheme":"log2x16","subbins":16,"min_ns":1000,"max_ns":100000000000},
  "classes": {"loss_tolerant": %s, "must_deliver": %s},
  "staleness_ns": %s,
  "update_gap_ns": %s,
  "raw": {"slots":1,"submitted":1,"recv_measured":1,"recv_unmeasured":0},
  "traffic": [
    {"traffic_id":1,"direction":"client_to_server","class":"must_deliver","deadline_ns":200000000,
     "slots":1,"slots_broadcast":0,"submitted":1,"delivered_unique":1,"duplicates":0,"deadline_hit":1,
     "expected_flows":0,"observed_flows":0,"never_received_flows":0,
     "latency_sched_ns":%s,"latency_send_ns":%s,"update_gap_ns":%s,"staleness_ns":%s}
  ]
}`, emptyClass, mustClass, emptyHist, emptyHist, oneHist, oneHist, emptyHist, emptyHist)
	path := writeMetricsFixture(t, dir, "mismatch.json", file)
	if _, err := MergeMetricsFiles([]string{path}, 1); err == nil ||
		!strings.Contains(err.Error(), "deadline_hit=0, want traffic sum=1") {
		t.Fatalf("expected deadline aggregate mismatch, got %v", err)
	}
}

func TestMergeMetricsRejectsMixedVersions(t *testing.T) {
	dir := t.TempDir()
	v1 := writeMetricsFixture(t, dir, "v1.json", metricsFixtureString(
		ClassCounts{}, ClassCounts{}, bins(nil), bins(nil), bins(nil), bins(nil), bins(nil), RawCounts{},
	))
	empty := histFixtureString(v2Bins(nil))
	class := v2ClassFixtureString(ClassCounts{}, v2Bins(nil), v2Bins(nil), v2Bins(nil))
	v2 := writeMetricsFixture(t, dir, "v2.json", fmt.Sprintf(`{
  "version": 2,
  "histogram": {"scheme":"log2x16","subbins":16,"min_ns":1000,"max_ns":100000000000},
  "classes": {"loss_tolerant": %s, "must_deliver": %s},
  "staleness_ns": %s,
  "update_gap_ns": %s,
  "raw": {"slots":0,"submitted":0,"recv_measured":0,"recv_unmeasured":0},
  "traffic": [
    {"traffic_id":1,"direction":"client_to_server","class":"loss_tolerant","deadline_ns":0,
     "slots":0,"slots_broadcast":0,"submitted":0,"delivered_unique":0,"duplicates":0,"deadline_hit":0,
	 "expected_flows":0,"observed_flows":0,"never_received_flows":0,
     "latency_sched_ns":%s,"latency_send_ns":%s,"update_gap_ns":%s,"staleness_ns":%s}
  ]
}`, class, class, empty, empty, empty, empty, empty, empty))

	if _, err := MergeMetricsFiles([]string{v1, v2}, 1); err == nil {
		t.Fatal("mixed v1/v2 metrics unexpectedly accepted")
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

func v2ClassFixtureString(counts ClassCounts, sched, send, update []uint64) string {
	return fmt.Sprintf(`{"slots":%d,"slots_broadcast":%d,"submitted":%d,"delivered_unique":%d,"duplicates":%d,"deadline_hit":%d,"expected_flows":%d,"observed_flows":%d,"never_received_flows":%d,"latency_sched_ns":%s,"latency_send_ns":%s,"update_gap_ns":%s}`,
		counts.Slots,
		counts.SlotsBroadcast,
		counts.Submitted,
		counts.DeliveredUnique,
		counts.Duplicates,
		counts.DeadlineHit,
		counts.ExpectedFlows,
		counts.ObservedFlows,
		counts.NeverReceivedFlows,
		histFixtureString(sched),
		histFixtureString(send),
		histFixtureString(update))
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

func v2Bins(counts map[int]uint64) []uint64 {
	out := make([]uint64, v2HistogramBins)
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
