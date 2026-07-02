package result

import (
	"encoding/binary"
	"fmt"
	"math"
	"os"
)

// LatencyHist layout constants — mirrors harness/metrics.h exactly.
const (
	ExactMaxUS  = 10_000
	FineMaxUS   = 1_000_000
	CoarseMaxUS = 60_000_000
	FineBinUS   = 100
	CoarseBinUS = 1_000

	ExactBins  = ExactMaxUS + 1                                        // 10001
	FineBins   = (FineMaxUS - ExactMaxUS + FineBinUS - 1) / FineBinUS  // 9900
	CoarseBins = (CoarseMaxUS - FineMaxUS + CoarseBinUS - 1) / CoarseBinUS // 59000
	BinCount   = ExactBins + FineBins + CoarseBins                     // 78901

	HistMagic   = 0x5453484C // 'LHST'
	HistVersion = 1
)

// headerSize is magic(4) + version(4) + count(8) + overflow(8) + max(8) + bin_count(8) = 40 bytes.
const headerSize = 4 + 4 + 8 + 8 + 8 + 8

// Histogram mirrors the C++ LatencyHist binary format.
type Histogram struct {
	Count    uint64
	Overflow uint64
	MaxUS    uint64
	Bins     [BinCount]uint64
}

// ReadHistogram reads a binary histogram file written by harness/metrics.cc.
func ReadHistogram(path string) (*Histogram, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	minSize := headerSize + 8*BinCount
	if len(data) < minSize {
		return nil, fmt.Errorf("%s: file too short (%d bytes)", path, len(data))
	}
	magic := binary.LittleEndian.Uint32(data[0:4])
	version := binary.LittleEndian.Uint32(data[4:8])
	count := binary.LittleEndian.Uint64(data[8:16])
	overflow := binary.LittleEndian.Uint64(data[16:24])
	maxUS := binary.LittleEndian.Uint64(data[24:32])
	binCount := binary.LittleEndian.Uint64(data[32:40])

	if magic != HistMagic {
		return nil, fmt.Errorf("%s: bad magic 0x%08x", path, magic)
	}
	if version != HistVersion {
		return nil, fmt.Errorf("%s: unsupported version %d", path, version)
	}
	if binCount != BinCount {
		return nil, fmt.Errorf("%s: bin_count %d != expected %d (LatencyHist layout drift?)",
			path, binCount, BinCount)
	}

	h := &Histogram{
		Count:    count,
		Overflow: overflow,
		MaxUS:    maxUS,
	}
	off := headerSize
	for i := 0; i < BinCount; i++ {
		h.Bins[i] = binary.LittleEndian.Uint64(data[off : off+8])
		off += 8
	}
	return h, nil
}

// MergeFrom adds all counts from other into h, taking the max of MaxUS.
func (h *Histogram) MergeFrom(other *Histogram) {
	h.Count += other.Count
	h.Overflow += other.Overflow
	if other.MaxUS > h.MaxUS {
		h.MaxUS = other.MaxUS
	}
	for i, c := range other.Bins {
		if c != 0 {
			h.Bins[i] += c
		}
	}
}

// BinUpperBoundUS returns the upper-bound microsecond value for a bin index.
// Mirrors LatencyHist::bin_upper_bound_us in metrics.cc.
func BinUpperBoundUS(index int) int {
	if index < ExactBins {
		return index
	}
	if index < ExactBins+FineBins {
		fineIndex := index - ExactBins
		return ExactMaxUS + (fineIndex+1)*FineBinUS
	}
	coarseIndex := index - ExactBins - FineBins
	return FineMaxUS + (coarseIndex+1)*CoarseBinUS
}

// PercentileUS returns the p-th percentile latency in microseconds.
// Mirrors LatencyHist::percentile_us in metrics.cc.
// ランク規約は nearest-rank（ceil(count*p)、[1,count] にクランプ）。
// metrics.cc / runner.cc の BoundedHistogram と同一規約に統一されている。
func (h *Histogram) PercentileUS(p float64) int {
	if h.Count == 0 {
		return 0
	}
	q := p
	if q < 0.0 {
		q = 0.0
	}
	if q > 1.0 {
		q = 1.0
	}
	target := uint64(math.Ceil(q * float64(h.Count)))
	if target < 1 {
		target = 1
	}
	if target > h.Count {
		target = h.Count
	}
	var seen uint64
	for i, c := range h.Bins {
		seen += c
		if seen >= target {
			return BinUpperBoundUS(i)
		}
	}
	return int(h.MaxUS)
}
