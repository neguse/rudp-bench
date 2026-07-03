package report

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func writeSweepDir(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	capacity := `{
 "seed": 1,
 "cells": [
  {"transport": "enet", "workload": "r20p128", "regime": "wired", "capacity": 48, "break_conns": 64, "break_cause": "staleness_p99=90ms over floor", "evaluated_points": 7},
  {"transport": "enet", "workload": "echo", "regime": "wired", "capacity": 1024, "range_limited": true, "evaluated_points": 11},
  {"transport": "gns", "workload": "r20p128", "regime": "wired", "capacity": 32, "censored": true, "break_cause": "farm_limited: attempted_ratio", "evaluated_points": 6},
  {"transport": "websocket", "workload": "r20p128", "regime": "wired", "capacity": 0, "break_conns": 1, "break_cause": "delivery_md=0.90 below 0.95", "evaluated_points": 1}
 ]
}`
	results := `{"transport":"enet","workload":"r20p128","regime":"wired","conns":48,"verdict":"VALID","judgment":{"ok":true,"staleness_p99_ns":80000000},"run_dir":"x"}
{"transport":"enet","workload":"echo","regime":"wired","conns":1024,"verdict":"VALID","judgment":{"ok":true,"staleness_p99_ns":40000000},"run_dir":"x"}
`
	if err := os.WriteFile(filepath.Join(dir, "capacity.json"), []byte(capacity), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "results.jsonl"), []byte(results), 0o644); err != nil {
		t.Fatal(err)
	}
	return dir
}

func TestCapacityTable(t *testing.T) {
	sd, err := LoadSweep(writeSweepDir(t))
	if err != nil {
		t.Fatal(err)
	}
	table := sd.CapacityTable(false)

	for _, want := range []string{
		"| workload | enet | gns | websocket |", // 存在する transport のみ、既定順
		"| r20p128 ⚓br | 48 (st) | ≥32 (farm) | 0 (md) |",
		"| echo (synthetic) | ≥1024 | — | — |",
	} {
		if !strings.Contains(table, want) {
			t.Fatalf("table missing %q:\n%s", want, table)
		}
	}

	anchorsOnly := sd.CapacityTable(true)
	if strings.Contains(anchorsOnly, "echo") || !strings.Contains(anchorsOnly, "r20p128") {
		t.Fatalf("anchors-only table wrong:\n%s", anchorsOnly)
	}
}

func TestAnchorVerdicts(t *testing.T) {
	sd, err := LoadSweep(writeSweepDir(t))
	if err != nil {
		t.Fatal(err)
	}
	v := sd.AnchorVerdicts()
	// enet r20p128 capacity=48 の staleness 80ms は br 予算 100ms 内
	if !strings.Contains(v, "| r20p128 ⚓br | enet | 80ms | 100ms | ✓ |") {
		t.Fatalf("verdicts wrong:\n%s", v)
	}
}

func TestReplaceSection(t *testing.T) {
	md := "prose\n<!-- generated:capacity-wired -->\nOLD\n<!-- /generated:capacity-wired -->\nmore prose\n"
	out, err := ReplaceSection(md, "capacity-wired", "NEW TABLE\n")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(out, "NEW TABLE") || strings.Contains(out, "OLD") {
		t.Fatalf("replace failed:\n%s", out)
	}
	if !strings.Contains(out, "prose") || !strings.Contains(out, "more prose") {
		t.Fatalf("prose damaged:\n%s", out)
	}
	// 再置換(冪等な運用)
	out2, err := ReplaceSection(out, "capacity-wired", "NEWER\n")
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(out2, "NEWER") || strings.Contains(out2, "NEW TABLE") {
		t.Fatalf("second replace failed:\n%s", out2)
	}

	if _, err := ReplaceSection(md, "missing-section", "X"); err == nil {
		t.Fatal("expected error for missing marker")
	}
}
