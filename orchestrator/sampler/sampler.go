package sampler

import (
	"bufio"
	"context"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/monotonic"
)

const (
	DefaultInterval            = 100 * time.Millisecond
	DefaultClockTicksPerSecond = int64(100)
)

type Sample struct {
	TimeNS     int64  `json:"time_ns"`
	PID        int    `json:"pid"`
	UTimeTicks uint64 `json:"utime_ticks"`
	STimeTicks uint64 `json:"stime_ticks"`
	CPUTimeNS  int64  `json:"cpu_time_ns"`
	RSSBytes   uint64 `json:"rss_bytes"`
}

type Series struct {
	PID     int      `json:"pid"`
	Samples []Sample `json:"samples"`
}

func (s Series) Window(startNS, stopNS int64) Series {
	out := Series{PID: s.PID}
	for _, sample := range s.Samples {
		if sample.TimeNS >= startNS && sample.TimeNS <= stopNS {
			out.Samples = append(out.Samples, sample)
		}
	}
	return out
}

func WindowAll(series map[int]Series, startNS, stopNS int64) map[int]Series {
	out := make(map[int]Series, len(series))
	for pid, s := range series {
		out[pid] = s.Window(startNS, stopNS)
	}
	return out
}

type Sampler struct {
	PIDs                []int
	Interval            time.Duration
	ClockTicksPerSecond int64
}

func Collect(ctx context.Context, pids []int, interval time.Duration) (map[int]Series, error) {
	return Sampler{PIDs: pids, Interval: interval}.Run(ctx)
}

func (s Sampler) Run(ctx context.Context) (map[int]Series, error) {
	interval := s.Interval
	if interval == 0 {
		interval = DefaultInterval
	}
	ticksPerSecond := s.ClockTicksPerSecond
	if ticksPerSecond == 0 {
		ticksPerSecond = DefaultClockTicksPerSecond
	}
	series := make(map[int]Series, len(s.PIDs))
	for _, pid := range s.PIDs {
		series[pid] = Series{PID: pid}
	}

	if err := sampleAll(series, s.PIDs, ticksPerSecond); err != nil {
		return series, err
	}

	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return series, nil
		case <-ticker.C:
			if err := sampleAll(series, s.PIDs, ticksPerSecond); err != nil {
				return series, err
			}
		}
	}
}

func Read(pid int) (Sample, error) {
	return ReadWithClockTicks(pid, DefaultClockTicksPerSecond)
}

func ReadWithClockTicks(pid int, ticksPerSecond int64) (Sample, error) {
	if ticksPerSecond <= 0 {
		return Sample{}, fmt.Errorf("clock ticks per second must be > 0")
	}
	nowNS, err := monotonic.NowNS()
	if err != nil {
		return Sample{}, fmt.Errorf("clock_gettime(CLOCK_MONOTONIC): %w", err)
	}
	statBytes, err := os.ReadFile(filepath.Join("/proc", strconv.Itoa(pid), "stat"))
	if err != nil {
		return Sample{}, fmt.Errorf("read /proc/%d/stat: %w", pid, err)
	}
	utime, stime, err := ParseStat(string(statBytes))
	if err != nil {
		return Sample{}, fmt.Errorf("parse /proc/%d/stat: %w", pid, err)
	}
	statusBytes, err := os.ReadFile(filepath.Join("/proc", strconv.Itoa(pid), "status"))
	if err != nil {
		return Sample{}, fmt.Errorf("read /proc/%d/status: %w", pid, err)
	}
	rss, err := ParseStatusRSS(string(statusBytes))
	if err != nil {
		return Sample{}, fmt.Errorf("parse /proc/%d/status: %w", pid, err)
	}
	return Sample{
		TimeNS:     nowNS,
		PID:        pid,
		UTimeTicks: utime,
		STimeTicks: stime,
		CPUTimeNS:  int64(utime+stime) * 1_000_000_000 / ticksPerSecond,
		RSSBytes:   rss,
	}, nil
}

func ParseStat(stat string) (utimeTicks, stimeTicks uint64, err error) {
	end := strings.LastIndex(stat, ")")
	if end < 0 {
		return 0, 0, fmt.Errorf("missing comm terminator")
	}
	fields := strings.Fields(stat[end+1:])
	if len(fields) <= 12 {
		return 0, 0, fmt.Errorf("too few fields after comm: %d", len(fields))
	}
	utime, err := strconv.ParseUint(fields[11], 10, 64)
	if err != nil {
		return 0, 0, fmt.Errorf("utime: %w", err)
	}
	stime, err := strconv.ParseUint(fields[12], 10, 64)
	if err != nil {
		return 0, 0, fmt.Errorf("stime: %w", err)
	}
	return utime, stime, nil
}

func ParseStatusRSS(status string) (uint64, error) {
	scanner := bufio.NewScanner(strings.NewReader(status))
	for scanner.Scan() {
		line := scanner.Text()
		if !strings.HasPrefix(line, "VmRSS:") {
			continue
		}
		fields := strings.Fields(line)
		if len(fields) < 2 {
			return 0, fmt.Errorf("VmRSS line has too few fields")
		}
		kb, err := strconv.ParseUint(fields[1], 10, 64)
		if err != nil {
			return 0, fmt.Errorf("VmRSS: %w", err)
		}
		return kb * 1024, nil
	}
	if err := scanner.Err(); err != nil {
		return 0, err
	}
	return 0, nil
}

func sampleAll(series map[int]Series, pids []int, ticksPerSecond int64) error {
	for _, pid := range pids {
		sample, err := ReadWithClockTicks(pid, ticksPerSecond)
		if err != nil {
			return err
		}
		current := series[pid]
		current.Samples = append(current.Samples, sample)
		series[pid] = current
	}
	return nil
}
