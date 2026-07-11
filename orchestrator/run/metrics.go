package run

import (
	"encoding/json"
	"fmt"
	"math"
	"os"
	"sort"
)

const (
	ClassLossTolerant = "loss_tolerant"
	ClassMustDeliver  = "must_deliver"
	v2HistogramBins   = 448
)

var metricClassNames = []string{ClassLossTolerant, ClassMustDeliver}

const (
	DirectionRoomRelay      = "room_relay"
	DirectionClientToServer = "client_to_server"
	DirectionServerToClient = "server_to_client"
)

type HistogramLayout struct {
	Scheme  string `json:"scheme"`
	Subbins uint   `json:"subbins"`
	MinNS   uint64 `json:"min_ns"`
	MaxNS   uint64 `json:"max_ns"`
}

type ClassCounts struct {
	Slots              uint64 `json:"slots"`
	SlotsBroadcast     uint64 `json:"slots_broadcast"`
	Submitted          uint64 `json:"submitted"`
	DeliveredUnique    uint64 `json:"delivered_unique"`
	Duplicates         uint64 `json:"duplicates"`
	DeadlineHit        uint64 `json:"deadline_hit"`
	ExpectedFlows      uint64 `json:"expected_flows"`
	ObservedFlows      uint64 `json:"observed_flows"`
	NeverReceivedFlows uint64 `json:"never_received_flows"`
}

type RawCounts struct {
	Slots          uint64 `json:"slots"`
	Submitted      uint64 `json:"submitted"`
	RecvMeasured   uint64 `json:"recv_measured"`
	RecvUnmeasured uint64 `json:"recv_unmeasured"`
}

type Histogram struct {
	Scheme string   `json:"scheme"`
	MinNS  uint64   `json:"min_ns"`
	MaxNS  uint64   `json:"max_ns"`
	Count  uint64   `json:"count"`
	P50NS  uint64   `json:"p50_ns"`
	P90NS  uint64   `json:"p90_ns"`
	P99NS  uint64   `json:"p99_ns"`
	Bins   []uint64 `json:"bins"`
}

type ClassAggregate struct {
	// update gap: latest-value が前進した受信同士の間隔(事象アライン指標、class 別)
	UpdateGapNS Histogram `json:"update_gap_ns"`

	ClassCounts
	ExpectedReceives uint64    `json:"expected_receives"`
	DeliveryRatio    float64   `json:"delivery_ratio"`
	AttemptedRatio   float64   `json:"attempted_ratio"`
	DeadlineHitRatio float64   `json:"deadline_hit_ratio"`
	LatencySchedNS   Histogram `json:"latency_sched_ns"`
	LatencySendNS    Histogram `json:"latency_send_ns"`
}

type TrafficKey struct {
	TrafficID uint8
	Direction string
	Class     string
}

type TrafficAggregate struct {
	TrafficID  uint8  `json:"traffic_id"`
	Direction  string `json:"direction"`
	Class      string `json:"class"`
	DeadlineNS uint64 `json:"deadline_ns"`

	ClassCounts
	ExpectedReceives uint64    `json:"expected_receives"`
	DeliveryRatio    float64   `json:"delivery_ratio"`
	AttemptedRatio   float64   `json:"attempted_ratio"`
	DeadlineHitRatio float64   `json:"deadline_hit_ratio"`
	LatencySchedNS   Histogram `json:"latency_sched_ns"`
	LatencySendNS    Histogram `json:"latency_send_ns"`
	UpdateGapNS      Histogram `json:"update_gap_ns"`
	StalenessNS      Histogram `json:"staleness_ns"`
}

type MergedMetrics struct {
	Version     int                       `json:"version"`
	TotalConns  int                       `json:"total_conns"`
	Histogram   HistogramLayout           `json:"histogram"`
	Classes     map[string]ClassAggregate `json:"classes"`
	StalenessNS Histogram                 `json:"staleness_ns"`
	// update gap: latest-value が前進した受信同士の間隔(事象アライン指標)
	UpdateGapNS Histogram          `json:"update_gap_ns"`
	Raw         RawCounts          `json:"raw"`
	Traffic     []TrafficAggregate `json:"traffic,omitempty"`

	trafficByKey map[TrafficKey]TrafficAggregate
}

type metricsFile struct {
	Version     int                    `json:"version"`
	Histogram   HistogramLayout        `json:"histogram"`
	Classes     map[string]classMetric `json:"classes"`
	StalenessNS Histogram              `json:"staleness_ns"`
	UpdateGapNS Histogram              `json:"update_gap_ns"`
	Raw         RawCounts              `json:"raw"`
	Traffic     []trafficMetric        `json:"traffic"`
}

type classMetric struct {
	ClassCounts
	LatencySchedNS Histogram `json:"latency_sched_ns"`
	LatencySendNS  Histogram `json:"latency_send_ns"`
	UpdateGapNS    Histogram `json:"update_gap_ns"`
}

type trafficMetric struct {
	TrafficID  uint8  `json:"traffic_id"`
	Direction  string `json:"direction"`
	Class      string `json:"class"`
	DeadlineNS uint64 `json:"deadline_ns"`
	ClassCounts
	LatencySchedNS Histogram `json:"latency_sched_ns"`
	LatencySendNS  Histogram `json:"latency_send_ns"`
	UpdateGapNS    Histogram `json:"update_gap_ns"`
	StalenessNS    Histogram `json:"staleness_ns"`
}

func MergeMetricsFiles(paths []string, totalConns int) (*MergedMetrics, error) {
	if len(paths) == 0 {
		return nil, fmt.Errorf("no metrics files to merge")
	}
	var merged *MergedMetrics
	expectedVersion := 0
	for _, path := range paths {
		file, err := readMetricsFile(path)
		if err != nil {
			return nil, err
		}
		if expectedVersion == 0 {
			expectedVersion = file.Version
		} else if file.Version != expectedVersion {
			return nil, fmt.Errorf("metrics %s version = %d, cannot mix with version %d", path, file.Version, expectedVersion)
		}
		if merged == nil {
			merged = newMergedMetrics(file.Version, file.Histogram, totalConns)
		} else if file.Histogram != merged.Histogram {
			return nil, fmt.Errorf("metrics %s histogram layout = %+v, want %+v", path, file.Histogram, merged.Histogram)
		}
		if file.Version > merged.Version {
			merged.Version = file.Version
		}
		if err := merged.add(file); err != nil {
			return nil, fmt.Errorf("merge metrics %s: %w", path, err)
		}
	}
	if err := merged.finalize(); err != nil {
		return nil, err
	}
	return merged, nil
}

func readMetricsFile(path string) (metricsFile, error) {
	return readMetricsFileWithOptions(path, false)
}

func readMetricsFileWithOptions(path string, allowEmptyTraffic bool) (metricsFile, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return metricsFile{}, fmt.Errorf("read metrics %s: %w", path, err)
	}
	var header struct {
		Version int `json:"version"`
	}
	if err := json.Unmarshal(data, &header); err != nil {
		return metricsFile{}, fmt.Errorf("decode metrics %s: %w", path, err)
	}
	if header.Version == 2 {
		if err := validateV2JSONShape(data, path); err != nil {
			return metricsFile{}, err
		}
	}
	var file metricsFile
	if err := json.Unmarshal(data, &file); err != nil {
		return metricsFile{}, fmt.Errorf("decode metrics %s: %w", path, err)
	}
	if err := validateMetricsFile(file, path, allowEmptyTraffic); err != nil {
		return metricsFile{}, err
	}
	return file, nil
}

var v2CountFields = []string{
	"slots", "slots_broadcast", "submitted", "delivered_unique", "duplicates", "deadline_hit",
	"expected_flows", "observed_flows", "never_received_flows",
}

var v2HistogramFields = []string{
	"scheme", "min_ns", "max_ns", "count", "p50_ns", "p90_ns", "p99_ns", "bins",
}

func validateV2JSONShape(data []byte, path string) error {
	top, err := requireJSONObject(data, "metrics "+path,
		"version", "histogram", "classes", "traffic", "staleness_ns", "raw")
	if err != nil {
		return err
	}
	if _, err := requireJSONObject(top["histogram"], "metrics "+path+" histogram",
		"scheme", "subbins", "min_ns", "max_ns"); err != nil {
		return err
	}
	classes, err := requireJSONObject(top["classes"], "metrics "+path+" classes", metricClassNames...)
	if err != nil {
		return err
	}
	for _, name := range metricClassNames {
		fields := append(append([]string{}, v2CountFields...), "latency_sched_ns", "latency_send_ns", "update_gap_ns")
		class, err := requireJSONObject(classes[name], "metrics "+path+" class "+name, fields...)
		if err != nil {
			return err
		}
		for _, histogram := range []string{"latency_sched_ns", "latency_send_ns", "update_gap_ns"} {
			if _, err := requireJSONObject(class[histogram], "metrics "+path+" class "+name+" "+histogram, v2HistogramFields...); err != nil {
				return err
			}
		}
	}
	if _, err := requireJSONObject(top["staleness_ns"], "metrics "+path+" staleness_ns", v2HistogramFields...); err != nil {
		return err
	}
	if _, err := requireJSONObject(top["raw"], "metrics "+path+" raw",
		"slots", "submitted", "recv_measured", "recv_unmeasured"); err != nil {
		return err
	}
	var traffic []json.RawMessage
	if err := json.Unmarshal(top["traffic"], &traffic); err != nil {
		return fmt.Errorf("metrics %s traffic must be an array: %w", path, err)
	}
	for i, raw := range traffic {
		fields := append(append([]string{}, v2CountFields...),
			"traffic_id", "direction", "class", "deadline_ns", "latency_sched_ns", "latency_send_ns", "update_gap_ns", "staleness_ns")
		context := fmt.Sprintf("metrics %s traffic[%d]", path, i)
		entry, err := requireJSONObject(raw, context, fields...)
		if err != nil {
			return err
		}
		for _, histogram := range []string{"latency_sched_ns", "latency_send_ns", "update_gap_ns", "staleness_ns"} {
			if _, err := requireJSONObject(entry[histogram], context+" "+histogram, v2HistogramFields...); err != nil {
				return err
			}
		}
	}
	return nil
}

func requireJSONObject(data []byte, context string, fields ...string) (map[string]json.RawMessage, error) {
	var object map[string]json.RawMessage
	if err := json.Unmarshal(data, &object); err != nil || object == nil {
		if err == nil {
			err = fmt.Errorf("value is not an object")
		}
		return nil, fmt.Errorf("%s: %w", context, err)
	}
	for _, field := range fields {
		value, ok := object[field]
		if !ok || string(value) == "null" {
			return nil, fmt.Errorf("%s missing required field %q", context, field)
		}
	}
	return object, nil
}

func validateMetricsFile(file metricsFile, path string, allowEmptyTraffic bool) error {
	if file.Version != 1 && file.Version != 2 {
		return fmt.Errorf("metrics %s version = %d, want 1 or 2", path, file.Version)
	}
	if file.Histogram.Scheme == "" || file.Histogram.Subbins == 0 || file.Histogram.MinNS == 0 || file.Histogram.MaxNS == 0 {
		return fmt.Errorf("metrics %s has incomplete histogram layout: %+v", path, file.Histogram)
	}
	if file.Classes == nil {
		return fmt.Errorf("metrics %s missing classes", path)
	}
	for _, name := range metricClassNames {
		metric, ok := file.Classes[name]
		if !ok {
			return fmt.Errorf("metrics %s missing class %q", path, name)
		}
		if err := validateClassCounts(metric.ClassCounts); err != nil {
			return fmt.Errorf("metrics %s class %q: %w", path, name, err)
		}
	}
	if file.Version == 2 && len(file.Traffic) == 0 && !allowEmptyTraffic {
		return fmt.Errorf("metrics %s version 2 has no traffic series", path)
	}
	if file.Version == 2 {
		if file.Histogram.Scheme != "log2x16" || file.Histogram.Subbins != 16 ||
			file.Histogram.MinNS != 1_000 || file.Histogram.MaxNS != 100_000_000_000 {
			return fmt.Errorf("metrics %s has non-v2 histogram layout: %+v", path, file.Histogram)
		}
		for name, metric := range file.Classes {
			for histName, histogram := range map[string]Histogram{
				"latency_sched_ns": metric.LatencySchedNS,
				"latency_send_ns":  metric.LatencySendNS,
				"update_gap_ns":    metric.UpdateGapNS,
			} {
				if err := validateV2Histogram(histogram, file.Histogram); err != nil {
					return fmt.Errorf("metrics %s class %s %s: %w", path, name, histName, err)
				}
			}
		}
		if err := validateV2Histogram(file.StalenessNS, file.Histogram); err != nil {
			return fmt.Errorf("metrics %s staleness_ns: %w", path, err)
		}
		// Top-level update_gap_ns is an optional diagnostic aggregate. Per-class
		// and per-traffic update gap histograms remain mandatory in version 2.
		if len(file.UpdateGapNS.Bins) > 0 {
			if err := validateV2Histogram(file.UpdateGapNS, file.Histogram); err != nil {
				return fmt.Errorf("metrics %s update_gap_ns: %w", path, err)
			}
		}
	}
	seenTraffic := map[TrafficKey]bool{}
	for i, traffic := range file.Traffic {
		if traffic.Direction != DirectionRoomRelay && traffic.Direction != DirectionClientToServer && traffic.Direction != DirectionServerToClient {
			return fmt.Errorf("metrics %s traffic[%d] has unknown direction %q", path, i, traffic.Direction)
		}
		if traffic.Class != ClassLossTolerant && traffic.Class != ClassMustDeliver {
			return fmt.Errorf("metrics %s traffic[%d] has unknown class %q", path, i, traffic.Class)
		}
		key := TrafficKey{TrafficID: traffic.TrafficID, Direction: traffic.Direction, Class: traffic.Class}
		if seenTraffic[key] {
			return fmt.Errorf("metrics %s has duplicate traffic key %+v", path, key)
		}
		seenTraffic[key] = true
		if err := validateClassCounts(traffic.ClassCounts); err != nil {
			return fmt.Errorf("metrics %s traffic[%d]: %w", path, i, err)
		}
		for histName, histogram := range map[string]Histogram{
			"latency_sched_ns": traffic.LatencySchedNS,
			"latency_send_ns":  traffic.LatencySendNS,
			"update_gap_ns":    traffic.UpdateGapNS,
			"staleness_ns":     traffic.StalenessNS,
		} {
			if err := validateV2Histogram(histogram, file.Histogram); err != nil {
				return fmt.Errorf("metrics %s traffic[%d] %s: %w", path, i, histName, err)
			}
		}
	}
	if file.Version == 2 {
		if err := validateV2AggregateConsistency(file); err != nil {
			return fmt.Errorf("metrics %s legacy aggregate: %w", path, err)
		}
	}
	return nil
}

func validateV2AggregateConsistency(file metricsFile) error {
	countSums := map[string]ClassCounts{
		ClassLossTolerant: {},
		ClassMustDeliver:  {},
	}
	latencySched := map[string][]Histogram{}
	latencySend := map[string][]Histogram{}
	updateGap := map[string][]Histogram{}
	var staleness []Histogram
	for _, traffic := range file.Traffic {
		countSums[traffic.Class] = addClassCounts(countSums[traffic.Class], traffic.ClassCounts)
		latencySched[traffic.Class] = append(latencySched[traffic.Class], traffic.LatencySchedNS)
		latencySend[traffic.Class] = append(latencySend[traffic.Class], traffic.LatencySendNS)
		updateGap[traffic.Class] = append(updateGap[traffic.Class], traffic.UpdateGapNS)
		if traffic.Class == ClassLossTolerant {
			staleness = append(staleness, traffic.StalenessNS)
		}
	}
	for _, class := range metricClassNames {
		aggregate := file.Classes[class]
		if field, got, want := firstClassCountMismatch(aggregate.ClassCounts, countSums[class]); field != "" {
			return fmt.Errorf("class %s %s=%d, want traffic sum=%d", class, field, got, want)
		}
		for _, check := range []struct {
			name      string
			aggregate Histogram
			parts     []Histogram
		}{
			{"latency_sched_ns", aggregate.LatencySchedNS, latencySched[class]},
			{"latency_send_ns", aggregate.LatencySendNS, latencySend[class]},
			{"update_gap_ns", aggregate.UpdateGapNS, updateGap[class]},
		} {
			if err := validateHistogramSum(check.aggregate, check.parts); err != nil {
				return fmt.Errorf("class %s %s: %w", class, check.name, err)
			}
		}
	}
	if err := validateHistogramSum(file.StalenessNS, staleness); err != nil {
		return fmt.Errorf("staleness_ns: %w", err)
	}
	return nil
}

// ValidateMergedMetricsConsistency reapplies the version-2 accounting contract
// to a persisted result.json. Rejudge uses it before applying new SLO logic.
func ValidateMergedMetricsConsistency(metrics *MergedMetrics) error {
	if metrics == nil || metrics.Version != 2 {
		return nil
	}
	countSums := map[string]ClassCounts{
		ClassLossTolerant: {},
		ClassMustDeliver:  {},
	}
	latencySched := map[string][]Histogram{}
	latencySend := map[string][]Histogram{}
	updateGap := map[string][]Histogram{}
	var staleness []Histogram
	for _, traffic := range metrics.Traffic {
		if traffic.Class != ClassLossTolerant && traffic.Class != ClassMustDeliver {
			return fmt.Errorf("unknown traffic class %q", traffic.Class)
		}
		countSums[traffic.Class] = addClassCounts(countSums[traffic.Class], traffic.ClassCounts)
		latencySched[traffic.Class] = append(latencySched[traffic.Class], traffic.LatencySchedNS)
		latencySend[traffic.Class] = append(latencySend[traffic.Class], traffic.LatencySendNS)
		updateGap[traffic.Class] = append(updateGap[traffic.Class], traffic.UpdateGapNS)
		if traffic.Class == ClassLossTolerant {
			staleness = append(staleness, traffic.StalenessNS)
		}
	}
	for _, class := range metricClassNames {
		aggregate, ok := metrics.Classes[class]
		if !ok {
			return fmt.Errorf("missing class %q", class)
		}
		if field, got, want := firstClassCountMismatch(aggregate.ClassCounts, countSums[class]); field != "" {
			return fmt.Errorf("class %s %s=%d, want traffic sum=%d", class, field, got, want)
		}
		for _, check := range []struct {
			name      string
			aggregate Histogram
			parts     []Histogram
		}{
			{"latency_sched_ns", aggregate.LatencySchedNS, latencySched[class]},
			{"latency_send_ns", aggregate.LatencySendNS, latencySend[class]},
			{"update_gap_ns", aggregate.UpdateGapNS, updateGap[class]},
		} {
			if err := validateHistogramSum(check.aggregate, check.parts); err != nil {
				return fmt.Errorf("class %s %s: %w", class, check.name, err)
			}
		}
	}
	if err := validateHistogramSum(metrics.StalenessNS, staleness); err != nil {
		return fmt.Errorf("staleness_ns: %w", err)
	}
	return nil
}

func firstClassCountMismatch(got, want ClassCounts) (string, uint64, uint64) {
	fields := []struct {
		name      string
		got, want uint64
	}{
		{"slots", got.Slots, want.Slots},
		{"slots_broadcast", got.SlotsBroadcast, want.SlotsBroadcast},
		{"submitted", got.Submitted, want.Submitted},
		{"delivered_unique", got.DeliveredUnique, want.DeliveredUnique},
		{"duplicates", got.Duplicates, want.Duplicates},
		{"deadline_hit", got.DeadlineHit, want.DeadlineHit},
		{"expected_flows", got.ExpectedFlows, want.ExpectedFlows},
		{"observed_flows", got.ObservedFlows, want.ObservedFlows},
		{"never_received_flows", got.NeverReceivedFlows, want.NeverReceivedFlows},
	}
	for _, field := range fields {
		if field.got != field.want {
			return field.name, field.got, field.want
		}
	}
	return "", 0, 0
}

func validateHistogramSum(aggregate Histogram, parts []Histogram) error {
	if len(aggregate.Bins) != v2HistogramBins {
		return fmt.Errorf("aggregate bins=%d, want %d", len(aggregate.Bins), v2HistogramBins)
	}
	expected := make([]uint64, len(aggregate.Bins))
	var expectedCount uint64
	for _, part := range parts {
		if len(part.Bins) != len(aggregate.Bins) {
			return fmt.Errorf("traffic bins=%d, want %d", len(part.Bins), len(aggregate.Bins))
		}
		expectedCount += part.Count
		for i, value := range part.Bins {
			expected[i] += value
		}
	}
	if aggregate.Count != expectedCount {
		return fmt.Errorf("count=%d, want traffic sum=%d", aggregate.Count, expectedCount)
	}
	for i, value := range aggregate.Bins {
		if value != expected[i] {
			return fmt.Errorf("bins[%d]=%d, want traffic sum=%d", i, value, expected[i])
		}
	}
	return nil
}

func validateV2Histogram(histogram Histogram, layout HistogramLayout) error {
	if histogram.Scheme != layout.Scheme || histogram.MinNS != layout.MinNS || histogram.MaxNS != layout.MaxNS {
		return fmt.Errorf("layout mismatch")
	}
	if len(histogram.Bins) != v2HistogramBins {
		return fmt.Errorf("bins=%d, want %d", len(histogram.Bins), v2HistogramBins)
	}
	var count uint64
	for _, value := range histogram.Bins {
		count += value
	}
	if histogram.Count != count {
		return fmt.Errorf("count=%d, want sum(bins)=%d", histogram.Count, count)
	}
	return nil
}

func validateClassCounts(c ClassCounts) error {
	if c.SlotsBroadcast > c.Slots {
		return fmt.Errorf("slots_broadcast=%d exceeds slots=%d", c.SlotsBroadcast, c.Slots)
	}
	if c.Submitted > c.Slots {
		return fmt.Errorf("submitted=%d exceeds slots=%d", c.Submitted, c.Slots)
	}
	if c.DeadlineHit > c.DeliveredUnique {
		return fmt.Errorf("deadline_hit=%d exceeds delivered_unique=%d", c.DeadlineHit, c.DeliveredUnique)
	}
	if c.ObservedFlows > c.ExpectedFlows {
		return fmt.Errorf("observed_flows=%d exceeds expected_flows=%d", c.ObservedFlows, c.ExpectedFlows)
	}
	if c.NeverReceivedFlows != c.ExpectedFlows-c.ObservedFlows {
		return fmt.Errorf("never_received_flows=%d, want expected-observed=%d", c.NeverReceivedFlows, c.ExpectedFlows-c.ObservedFlows)
	}
	return nil
}

func newMergedMetrics(version int, layout HistogramLayout, totalConns int) *MergedMetrics {
	out := &MergedMetrics{
		Version:      version,
		TotalConns:   totalConns,
		Histogram:    layout,
		Classes:      make(map[string]ClassAggregate, len(metricClassNames)),
		trafficByKey: make(map[TrafficKey]TrafficAggregate),
	}
	for _, name := range metricClassNames {
		out.Classes[name] = ClassAggregate{}
	}
	return out
}

func (m *MergedMetrics) add(file metricsFile) error {
	for _, name := range metricClassNames {
		src := file.Classes[name]
		dst := m.Classes[name]
		dst.ClassCounts = addClassCounts(dst.ClassCounts, src.ClassCounts)
		if err := addHistogram(&dst.LatencySchedNS, src.LatencySchedNS, m.Histogram); err != nil {
			return fmt.Errorf("%s latency_sched_ns: %w", name, err)
		}
		if err := addHistogram(&dst.LatencySendNS, src.LatencySendNS, m.Histogram); err != nil {
			return fmt.Errorf("%s latency_send_ns: %w", name, err)
		}
		// update_gap_ns を出さない実装(移行中の client)は欠落を許容する
		if len(src.UpdateGapNS.Bins) > 0 {
			if err := addHistogram(&dst.UpdateGapNS, src.UpdateGapNS, m.Histogram); err != nil {
				return fmt.Errorf("%s update_gap_ns: %w", name, err)
			}
		}
		m.Classes[name] = dst
	}
	if err := addHistogram(&m.StalenessNS, file.StalenessNS, m.Histogram); err != nil {
		return fmt.Errorf("staleness_ns: %w", err)
	}
	// update_gap_ns を出さない実装(移行中の client)は欠落を許容する
	if len(file.UpdateGapNS.Bins) > 0 {
		if err := addHistogram(&m.UpdateGapNS, file.UpdateGapNS, m.Histogram); err != nil {
			return fmt.Errorf("update_gap_ns: %w", err)
		}
	}
	for _, src := range file.Traffic {
		key := TrafficKey{TrafficID: src.TrafficID, Direction: src.Direction, Class: src.Class}
		dst := m.trafficByKey[key]
		if dst.Direction == "" {
			dst.TrafficID = src.TrafficID
			dst.Direction = src.Direction
			dst.Class = src.Class
			dst.DeadlineNS = src.DeadlineNS
		} else if dst.DeadlineNS != src.DeadlineNS {
			return fmt.Errorf("traffic %+v deadline_ns=%d differs from %d", key, src.DeadlineNS, dst.DeadlineNS)
		}
		dst.ClassCounts = addClassCounts(dst.ClassCounts, src.ClassCounts)
		if err := addHistogram(&dst.LatencySchedNS, src.LatencySchedNS, m.Histogram); err != nil {
			return fmt.Errorf("traffic %+v latency_sched_ns: %w", key, err)
		}
		if err := addHistogram(&dst.LatencySendNS, src.LatencySendNS, m.Histogram); err != nil {
			return fmt.Errorf("traffic %+v latency_send_ns: %w", key, err)
		}
		if len(src.UpdateGapNS.Bins) > 0 {
			if err := addHistogram(&dst.UpdateGapNS, src.UpdateGapNS, m.Histogram); err != nil {
				return fmt.Errorf("traffic %+v update_gap_ns: %w", key, err)
			}
		}
		if len(src.StalenessNS.Bins) > 0 {
			if err := addHistogram(&dst.StalenessNS, src.StalenessNS, m.Histogram); err != nil {
				return fmt.Errorf("traffic %+v staleness_ns: %w", key, err)
			}
		}
		m.trafficByKey[key] = dst
	}
	m.Raw.Slots += file.Raw.Slots
	m.Raw.Submitted += file.Raw.Submitted
	m.Raw.RecvMeasured += file.Raw.RecvMeasured
	m.Raw.RecvUnmeasured += file.Raw.RecvUnmeasured
	return nil
}

func (m *MergedMetrics) finalize() error {
	for _, name := range metricClassNames {
		c := m.Classes[name]
		if c.SlotsBroadcast > c.Slots {
			return fmt.Errorf("%s slots_broadcast=%d exceeds slots=%d", name, c.SlotsBroadcast, c.Slots)
		}
		c.ExpectedReceives = expectedReceives(c.Slots, c.SlotsBroadcast, m.TotalConns)
		if c.DeliveredUnique > c.ExpectedReceives {
			return fmt.Errorf("%s delivered_unique=%d exceeds expected_receives=%d", name, c.DeliveredUnique, c.ExpectedReceives)
		}
		c.DeliveryRatio = ratioOrOne(c.DeliveredUnique, c.ExpectedReceives)
		c.AttemptedRatio = ratioOrOne(c.Submitted, c.Slots)
		c.DeadlineHitRatio = ratioOrOne(c.DeadlineHit, c.ExpectedReceives)
		finalizeHistogram(&c.LatencySchedNS, m.Histogram)
		finalizeHistogram(&c.LatencySendNS, m.Histogram)
		finalizeHistogram(&c.UpdateGapNS, m.Histogram)
		m.Classes[name] = c
	}
	finalizeHistogram(&m.StalenessNS, m.Histogram)
	finalizeHistogram(&m.UpdateGapNS, m.Histogram)
	keys := make([]TrafficKey, 0, len(m.trafficByKey))
	for key := range m.trafficByKey {
		keys = append(keys, key)
	}
	sort.Slice(keys, func(i, j int) bool {
		if keys[i].TrafficID != keys[j].TrafficID {
			return keys[i].TrafficID < keys[j].TrafficID
		}
		if keys[i].Direction != keys[j].Direction {
			return keys[i].Direction < keys[j].Direction
		}
		return keys[i].Class < keys[j].Class
	})
	m.Traffic = make([]TrafficAggregate, 0, len(keys))
	for _, key := range keys {
		t := m.trafficByKey[key]
		if t.SlotsBroadcast > t.Slots {
			return fmt.Errorf("traffic %+v slots_broadcast=%d exceeds slots=%d", key, t.SlotsBroadcast, t.Slots)
		}
		t.ExpectedReceives = expectedReceives(t.Slots, t.SlotsBroadcast, m.TotalConns)
		if t.DeliveredUnique > t.ExpectedReceives {
			return fmt.Errorf("traffic %+v delivered_unique=%d exceeds expected_receives=%d", key, t.DeliveredUnique, t.ExpectedReceives)
		}
		t.DeliveryRatio = ratioOrOne(t.DeliveredUnique, t.ExpectedReceives)
		t.AttemptedRatio = ratioOrOne(t.Submitted, t.Slots)
		t.DeadlineHitRatio = ratioOrOne(t.DeadlineHit, t.ExpectedReceives)
		finalizeHistogram(&t.LatencySchedNS, m.Histogram)
		finalizeHistogram(&t.LatencySendNS, m.Histogram)
		finalizeHistogram(&t.UpdateGapNS, m.Histogram)
		finalizeHistogram(&t.StalenessNS, m.Histogram)
		m.trafficByKey[key] = t
		m.Traffic = append(m.Traffic, t)
	}
	return nil
}

func (m *MergedMetrics) TrafficMetric(trafficID uint8, direction, class string) (TrafficAggregate, bool) {
	if m == nil {
		return TrafficAggregate{}, false
	}
	t, ok := m.trafficByKey[TrafficKey{TrafficID: trafficID, Direction: direction, Class: class}]
	if ok {
		return t, true
	}
	for _, candidate := range m.Traffic {
		if candidate.TrafficID == trafficID && candidate.Direction == direction && candidate.Class == class {
			return candidate, true
		}
	}
	return TrafficAggregate{}, false
}

func (m *MergedMetrics) attemptedRatio() float64 {
	var slots, submitted uint64
	for _, name := range metricClassNames {
		c := m.Classes[name]
		slots += c.Slots
		submitted += c.Submitted
	}
	return ratioOrOne(submitted, slots)
}

func addClassCounts(a, b ClassCounts) ClassCounts {
	return ClassCounts{
		Slots:              a.Slots + b.Slots,
		SlotsBroadcast:     a.SlotsBroadcast + b.SlotsBroadcast,
		Submitted:          a.Submitted + b.Submitted,
		DeliveredUnique:    a.DeliveredUnique + b.DeliveredUnique,
		Duplicates:         a.Duplicates + b.Duplicates,
		DeadlineHit:        a.DeadlineHit + b.DeadlineHit,
		ExpectedFlows:      a.ExpectedFlows + b.ExpectedFlows,
		ObservedFlows:      a.ObservedFlows + b.ObservedFlows,
		NeverReceivedFlows: a.NeverReceivedFlows + b.NeverReceivedFlows,
	}
}

func addHistogram(dst *Histogram, src Histogram, layout HistogramLayout) error {
	if src.Scheme != layout.Scheme || src.MinNS != layout.MinNS || src.MaxNS != layout.MaxNS {
		return fmt.Errorf("layout = {scheme:%q min:%d max:%d}, want {scheme:%q min:%d max:%d}", src.Scheme, src.MinNS, src.MaxNS, layout.Scheme, layout.MinNS, layout.MaxNS)
	}
	if len(src.Bins) == 0 {
		return fmt.Errorf("missing bins")
	}
	if dst.Bins == nil {
		dst.Scheme = layout.Scheme
		dst.MinNS = layout.MinNS
		dst.MaxNS = layout.MaxNS
		dst.Bins = make([]uint64, len(src.Bins))
	}
	if len(dst.Bins) != len(src.Bins) {
		return fmt.Errorf("bins len = %d, want %d", len(src.Bins), len(dst.Bins))
	}
	for i, v := range src.Bins {
		dst.Bins[i] += v
	}
	dst.Count += src.Count
	return nil
}

func finalizeHistogram(h *Histogram, layout HistogramLayout) {
	if h.Bins == nil {
		h.Scheme = layout.Scheme
		h.MinNS = layout.MinNS
		h.MaxNS = layout.MaxNS
		h.Bins = []uint64{}
	}
	var count uint64
	for _, v := range h.Bins {
		count += v
	}
	h.Count = count
	h.P50NS = histogramPercentile(*h, layout, 0.50)
	h.P90NS = histogramPercentile(*h, layout, 0.90)
	h.P99NS = histogramPercentile(*h, layout, 0.99)
}

func histogramPercentile(h Histogram, layout HistogramLayout, p float64) uint64 {
	if h.Count == 0 {
		return 0
	}
	if p < 0 {
		p = 0
	} else if p > 1 {
		p = 1
	}
	rank := uint64(math.Ceil(float64(h.Count) * p))
	if rank == 0 {
		rank = 1
	}
	var cumulative uint64
	for i, v := range h.Bins {
		cumulative += v
		if cumulative >= rank {
			return histogramBinUpperNS(layout, i, len(h.Bins))
		}
	}
	return layout.MaxNS
}

func histogramBinUpperNS(layout HistogramLayout, index, bins int) uint64 {
	if index <= 0 {
		return layout.MinNS
	}
	if index >= bins-1 {
		return layout.MaxNS
	}
	major := uint(index) / layout.Subbins
	sub := uint(index) % layout.Subbins
	low := layout.MinNS
	for i := uint(0); i < major; i++ {
		if low > math.MaxUint64/2 {
			low = math.MaxUint64
			break
		}
		low *= 2
	}
	upper := low + ((low*uint64(sub+1) + uint64(layout.Subbins) - 1) / uint64(layout.Subbins))
	if upper > layout.MaxNS {
		return layout.MaxNS
	}
	return upper
}

func expectedReceives(slots, broadcastSlots uint64, totalConns int) uint64 {
	if totalConns < 0 {
		totalConns = 0
	}
	return (slots - broadcastSlots) + broadcastSlots*uint64(totalConns)
}

func ratioOrOne(num, den uint64) float64 {
	if den == 0 {
		return 1
	}
	return float64(num) / float64(den)
}
