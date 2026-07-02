package sampler_test

import (
	"context"
	"os"
	"testing"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/monotonic"
	"github.com/neguse/rudp-bench/orchestrator/sampler"
)

func TestParseStatAndStatus(t *testing.T) {
	utime, stime, err := sampler.ParseStat("123 (cmd with space) S 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15")
	if err != nil {
		t.Fatal(err)
	}
	if utime != 11 || stime != 12 {
		t.Fatalf("utime/stime = %d/%d, want 11/12", utime, stime)
	}
	rss, err := sampler.ParseStatusRSS("Name:\ttest\nVmRSS:\t  42 kB\n")
	if err != nil {
		t.Fatal(err)
	}
	if rss != 42*1024 {
		t.Fatalf("rss = %d, want %d", rss, 42*1024)
	}
}

func TestSelfSampleSmoke(t *testing.T) {
	sample, err := sampler.Read(os.Getpid())
	if err != nil {
		t.Fatal(err)
	}
	if sample.PID != os.Getpid() {
		t.Fatalf("pid = %d, want %d", sample.PID, os.Getpid())
	}
	if sample.TimeNS <= 0 {
		t.Fatalf("time_ns = %d", sample.TimeNS)
	}
	if sample.RSSBytes == 0 {
		t.Fatal("RSSBytes = 0")
	}
}

func TestCollectAndWindowSmoke(t *testing.T) {
	start, err := monotonic.NowNS()
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 140*time.Millisecond)
	defer cancel()
	series, err := sampler.Collect(ctx, []int{os.Getpid()}, 30*time.Millisecond)
	if err != nil {
		t.Fatal(err)
	}
	stop, err := monotonic.NowNS()
	if err != nil {
		t.Fatal(err)
	}
	got := series[os.Getpid()]
	if len(got.Samples) < 2 {
		t.Fatalf("samples = %d, want at least 2", len(got.Samples))
	}
	windowed := got.Window(start, stop)
	if len(windowed.Samples) == 0 {
		t.Fatal("windowed samples = 0")
	}
}
