package main

import (
	"path/filepath"
	"testing"
)

func TestParseConfigKeepsEnvCompatibility(t *testing.T) {
	t.Setenv("OUT", "results/custom")
	t.Setenv("BUILD_DIR", "build-custom")
	t.Setenv("PUBLISH_ID", "custom-publish")
	t.Setenv("PUBLISH_DOCS", "0")
	t.Setenv("UPDATE_SUBMODULES", "0")
	t.Setenv("BENCH_ISOLATE", "0")
	t.Setenv("DRY_RUN", "1")
	t.Setenv("JOBS", "3")

	cfg, err := parseConfig(nil)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.out != filepath.Join(cfg.root, "results/custom") {
		t.Fatalf("out = %q", cfg.out)
	}
	if cfg.buildDir != filepath.Join(cfg.root, "build-custom") {
		t.Fatalf("buildDir = %q", cfg.buildDir)
	}
	if cfg.publishID != "custom-publish" {
		t.Fatalf("publishID = %q", cfg.publishID)
	}
	if cfg.updateSubmodules || cfg.publishDocs || cfg.benchIsolate || !cfg.dryRun {
		t.Fatalf("unexpected bool config: %+v", cfg)
	}
	if cfg.jobs != 3 {
		t.Fatalf("jobs = %d", cfg.jobs)
	}
}

func TestParseConfigNoFlags(t *testing.T) {
	cfg, err := parseConfig([]string{
		"--no-submodule-update",
		"--no-publish-docs",
		"--no-bench-isolate",
		"--dry-run",
	})
	if err != nil {
		t.Fatal(err)
	}
	if cfg.updateSubmodules || cfg.publishDocs || cfg.benchIsolate || !cfg.dryRun {
		t.Fatalf("unexpected bool config: %+v", cfg)
	}
}
