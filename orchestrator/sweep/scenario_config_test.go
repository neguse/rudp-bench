package sweep

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestLoadConfigScenario(t *testing.T) {
	path := filepath.Join(t.TempDir(), "scenario.json")
	data := []byte(`{
  "regime":"local",
  "transports":{"raw_udp":{"server_command":["server"],"client_command":["client"],"client_procs":1}},
  "scenarios":[{
    "name":"authoritative-smoke","kind":"authoritative_state",
	    "client_input":{"traffic_id":1,"loss_tolerant":{"rate_hz":10,"payload_bytes":64,"min_delivery_ratio":0.99},"must_deliver":{"rate_hz":1,"payload_bytes":64,"deadline_ns":100000000,"min_deadline_hit_ratio":0.99}},
	    "server_state":{"traffic_id":2,"loss_tolerant":{"rate_hz":20,"payload_bytes":64,"staleness_p99_ns":100000000},"must_deliver":{"rate_hz":1,"payload_bytes":64,"deadline_ns":100000000,"min_eventual_delivery_ratio":0.99}}
  }],
  "conns":{"min":1,"max":4},"output_dir":"out"
}`)
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}
	cfg, err := LoadConfig(path)
	if err != nil {
		t.Fatal(err)
	}
	if len(cfg.Scenarios) != 1 || cfg.Scenarios[0].Name != "authoritative-smoke" {
		t.Fatalf("scenarios = %+v", cfg.Scenarios)
	}
}

func TestLoadConfigRejectsMixedWorkloadAndScenario(t *testing.T) {
	path := filepath.Join(t.TempDir(), "mixed.json")
	data := []byte(`{
  "regime":"local","transports":{"t":{"server_command":["s"],"client_command":["c"],"client_procs":1}},
  "workloads":["echo"],
  "scenarios":[{"name":"baseline","kind":"environment_baseline","client_input":{"traffic_id":1,"loss_tolerant":{"rate_hz":1,"payload_bytes":32},"must_deliver":{"rate_hz":0,"payload_bytes":0}}}],
  "conns":{"min":1,"max":1},"output_dir":"out"
}`)
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := LoadConfig(path); err == nil {
		t.Fatal("expected mixed workloads/scenarios error")
	}
}

func TestLoadConfigRejectsUnknownField(t *testing.T) {
	path := filepath.Join(t.TempDir(), "unknown.json")
	data := []byte(`{
  "regime":"local","transports":{"t":{"server_command":["s"],"client_command":["c"],"client_procs":1}},
  "workloads":["echo"],"conns":{"min":1,"max":1},"output_dir":"out","duraton":"2s"
}`)
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := LoadConfig(path); err == nil {
		t.Fatal("unknown sweep field unexpectedly accepted")
	}
}

func TestLoadConfigReferenceRequiresDoctorReport(t *testing.T) {
	path := filepath.Join(t.TempDir(), "reference.json")
	data := []byte(`{
  "measurement_mode":"reference","regime":"local",
  "transports":{"t":{"server_command":["s"],"client_command":["c"],"client_procs":1}},
  "workloads":["echo"],"conns":{"min":1,"max":1},"output_dir":"out",` + baselineJSON + `
}`)
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := LoadConfig(path); err == nil || !strings.Contains(err.Error(), "doctor_report") {
		t.Fatalf("reference preflight error = %v", err)
	}
}
