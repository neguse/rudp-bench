package run

import (
	"encoding/json"
	"fmt"
	"math"
	"os"
)

const (
	ClassLossTolerant = "loss_tolerant"
	ClassMustDeliver  = "must_deliver"
)

var metricClassNames = []string{ClassLossTolerant, ClassMustDeliver}

type HistogramLayout struct {
	Scheme  string `json:"scheme"`
	Subbins uint   `json:"subbins"`
	MinNS   uint64 `json:"min_ns"`
	MaxNS   uint64 `json:"max_ns"`
}

type ClassCounts struct {
	Slots           uint64 `json:"slots"`
	SlotsBroadcast  uint64 `json:"slots_broadcast"`
	Submitted       uint64 `json:"submitted"`
	DeliveredUnique uint64 `json:"delivered_unique"`
	Duplicates      uint64 `json:"duplicates"`
	DeadlineHit     uint64 `json:"deadline_hit"`
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

type MergedMetrics struct {
	Version     int                       `json:"version"`
	TotalConns  int                       `json:"total_conns"`
	Histogram   HistogramLayout           `json:"histogram"`
	Classes     map[string]ClassAggregate `json:"classes"`
	StalenessNS Histogram                 `json:"staleness_ns"`
	// update gap: latest-value が前進した受信同士の間隔(事象アライン指標)
	UpdateGapNS Histogram `json:"update_gap_ns"`
	Raw         RawCounts `json:"raw"`
}

type metricsFile struct {
	Version     int                    `json:"version"`
	Histogram   HistogramLayout        `json:"histogram"`
	Classes     map[string]classMetric `json:"classes"`
	StalenessNS Histogram              `json:"staleness_ns"`
	UpdateGapNS Histogram              `json:"update_gap_ns"`
	Raw         RawCounts              `json:"raw"`
}

type classMetric struct {
	ClassCounts
	LatencySchedNS Histogram `json:"latency_sched_ns"`
	LatencySendNS  Histogram `json:"latency_send_ns"`
	UpdateGapNS    Histogram `json:"update_gap_ns"`
}

func MergeMetricsFiles(paths []string, totalConns int) (*MergedMetrics, error) {
	if len(paths) == 0 {
		return nil, fmt.Errorf("no metrics files to merge")
	}
	var merged *MergedMetrics
	for _, path := range paths {
		data, err := os.ReadFile(path)
		if err != nil {
			return nil, fmt.Errorf("read metrics %s: %w", path, err)
		}
		var file metricsFile
		if err := json.Unmarshal(data, &file); err != nil {
			return nil, fmt.Errorf("decode metrics %s: %w", path, err)
		}
		if err := validateMetricsFile(file, path); err != nil {
			return nil, err
		}
		if merged == nil {
			merged = newMergedMetrics(file.Histogram, totalConns)
		} else if file.Histogram != merged.Histogram {
			return nil, fmt.Errorf("metrics %s histogram layout = %+v, want %+v", path, file.Histogram, merged.Histogram)
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

func validateMetricsFile(file metricsFile, path string) error {
	if file.Version != 1 {
		return fmt.Errorf("metrics %s version = %d, want 1", path, file.Version)
	}
	if file.Histogram.Scheme == "" || file.Histogram.Subbins == 0 || file.Histogram.MinNS == 0 || file.Histogram.MaxNS == 0 {
		return fmt.Errorf("metrics %s has incomplete histogram layout: %+v", path, file.Histogram)
	}
	if file.Classes == nil {
		return fmt.Errorf("metrics %s missing classes", path)
	}
	for _, name := range metricClassNames {
		if _, ok := file.Classes[name]; !ok {
			return fmt.Errorf("metrics %s missing class %q", path, name)
		}
	}
	return nil
}

func newMergedMetrics(layout HistogramLayout, totalConns int) *MergedMetrics {
	out := &MergedMetrics{
		Version:    1,
		TotalConns: totalConns,
		Histogram:  layout,
		Classes:    make(map[string]ClassAggregate, len(metricClassNames)),
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
	return nil
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
		Slots:           a.Slots + b.Slots,
		SlotsBroadcast:  a.SlotsBroadcast + b.SlotsBroadcast,
		Submitted:       a.Submitted + b.Submitted,
		DeliveredUnique: a.DeliveredUnique + b.DeliveredUnique,
		Duplicates:      a.Duplicates + b.Duplicates,
		DeadlineHit:     a.DeadlineHit + b.DeadlineHit,
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
