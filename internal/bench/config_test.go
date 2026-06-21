package bench

import (
	"os"
	"path/filepath"
	"testing"
)

func TestLoadScenario(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "scenario.json")
	data := []byte(`{
		"locked": true,
		"libs": ["enet", "kcp"],
		"profiles": ["media_relay"],
		"runs": 3,
		"duration": 10,
		"tail_ms": 1000,
		"netem": true,
		"netem_args": "25 5 1 100000",
		"isolate": "systemd",
		"server_cpu": "7,15",
		"client_cpu": "3,4,5,6",
		"min_valid": 2,
		"min_delivery": 0.99,
		"build": true,
		"publish": true,
		"media_conns": "1 5 50"
	}`)
	if err := os.WriteFile(path, data, 0644); err != nil {
		t.Fatal(err)
	}

	s, err := LoadScenario(path)
	if err != nil {
		t.Fatal(err)
	}

	if !s.Locked {
		t.Error("expected Locked=true")
	}
	if len(s.Libs) != 2 || s.Libs[0] != "enet" || s.Libs[1] != "kcp" {
		t.Errorf("Libs = %v, want [enet kcp]", s.Libs)
	}
	if s.Runs != 3 {
		t.Errorf("Runs = %d, want 3", s.Runs)
	}
	if s.Duration != 10 {
		t.Errorf("Duration = %d, want 10", s.Duration)
	}
	if s.TailMs != 1000 {
		t.Errorf("TailMs = %d, want 1000", s.TailMs)
	}
	if !s.Netem {
		t.Error("expected Netem=true")
	}
	if s.Isolate != "systemd" {
		t.Errorf("Isolate = %q, want %q", s.Isolate, "systemd")
	}
	if s.MinDelivery != 0.99 {
		t.Errorf("MinDelivery = %f, want 0.99", s.MinDelivery)
	}
	if s.MediaConns != "1 5 50" {
		t.Errorf("MediaConns = %q, want %q", s.MediaConns, "1 5 50")
	}
}

func TestLoadScenarioDefaults(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "empty.json")
	if err := os.WriteFile(path, []byte(`{}`), 0644); err != nil {
		t.Fatal(err)
	}

	s, err := LoadScenario(path)
	if err != nil {
		t.Fatal(err)
	}

	if s.Duration != 5 {
		t.Errorf("default Duration = %d, want 5", s.Duration)
	}
	if s.TailMs != 500 {
		t.Errorf("default TailMs = %d, want 500", s.TailMs)
	}
	if s.Runs != 1 {
		t.Errorf("default Runs = %d, want 1", s.Runs)
	}
	if s.MinDelivery != 0.95 {
		t.Errorf("default MinDelivery = %f, want 0.95", s.MinDelivery)
	}
	if s.Isolate != "taskset" {
		t.Errorf("default Isolate = %q, want %q", s.Isolate, "taskset")
	}
	if len(s.Libs) != 14 {
		t.Errorf("default Libs count = %d, want 14", len(s.Libs))
	}
}

func TestDefaultScenario(t *testing.T) {
	s := DefaultScenario()
	if s.Duration != 5 {
		t.Errorf("Duration = %d, want 5", s.Duration)
	}
	if s.TailMs != 500 {
		t.Errorf("TailMs = %d, want 500", s.TailMs)
	}
	if s.Runs != 1 {
		t.Errorf("Runs = %d, want 1", s.Runs)
	}
	if s.MinDelivery != 0.95 {
		t.Errorf("MinDelivery = %f, want 0.95", s.MinDelivery)
	}
	if s.Isolate != "taskset" {
		t.Errorf("Isolate = %q, want %q", s.Isolate, "taskset")
	}
	if len(s.Libs) != 14 {
		t.Errorf("Libs count = %d, want 14", len(s.Libs))
	}
}

func TestLoadScenarioNotFound(t *testing.T) {
	_, err := LoadScenario("/nonexistent/path.json")
	if err == nil {
		t.Error("expected error for nonexistent file")
	}
}

func TestLoadScenarioInvalidJSON(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "bad.json")
	if err := os.WriteFile(path, []byte(`{invalid}`), 0644); err != nil {
		t.Fatal(err)
	}
	_, err := LoadScenario(path)
	if err == nil {
		t.Error("expected error for invalid JSON")
	}
}
