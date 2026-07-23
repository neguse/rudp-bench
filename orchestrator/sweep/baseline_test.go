package sweep

import (
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/neguse/rudp-bench/orchestrator/run"
)

func passingBaseline(phase string, delivery float64, stalenessNS uint64) BaselineRecord {
	return BaselineRecord{
		Phase: phase, Outcome: run.OutcomePass, Verdict: "VALID", OK: true,
		DeliveryRatio: delivery, StalenessP99NS: stalenessNS,
	}
}

func TestEvaluateDriftWithinTolerance(t *testing.T) {
	tol := DriftTolerance{MaxDeliveryDelta: 0.01, MaxStalenessP99Ratio: 1.5}
	ok, cause := evaluateDrift(
		passingBaseline("before", 0.980, 70_000_000),
		passingBaseline("after", 0.985, 80_000_000), tol)
	if !ok || cause != "" {
		t.Fatalf("drift ok=%v cause=%q", ok, cause)
	}
}

func TestEvaluateDriftDeliveryDelta(t *testing.T) {
	tol := DriftTolerance{MaxDeliveryDelta: 0.01, MaxStalenessP99Ratio: 1.5}
	ok, cause := evaluateDrift(
		passingBaseline("before", 0.999, 70_000_000),
		passingBaseline("after", 0.950, 70_000_000), tol)
	if ok || !strings.Contains(cause, "delivery") {
		t.Fatalf("drift ok=%v cause=%q", ok, cause)
	}
}

func TestEvaluateDriftStalenessRatio(t *testing.T) {
	tol := DriftTolerance{MaxDeliveryDelta: 0.05, MaxStalenessP99Ratio: 1.5}
	ok, cause := evaluateDrift(
		passingBaseline("before", 0.98, 50_000_000),
		passingBaseline("after", 0.98, 120_000_000), tol)
	if ok || !strings.Contains(cause, "staleness") {
		t.Fatalf("drift ok=%v cause=%q", ok, cause)
	}
}

func TestEvaluateDriftRequiresPassingBaselines(t *testing.T) {
	tol := DriftTolerance{MaxDeliveryDelta: 0.05, MaxStalenessP99Ratio: 1.5}
	bad := passingBaseline("after", 0.98, 50_000_000)
	bad.OK = false
	bad.Cause = "scenario SLO failed"
	ok, cause := evaluateDrift(passingBaseline("before", 0.98, 50_000_000), bad, tol)
	if ok || !strings.Contains(cause, "after") {
		t.Fatalf("drift ok=%v cause=%q", ok, cause)
	}
}

const baselineJSON = `"baseline":{
  "transport":"raw_udp",
  "server_command":["srv"],"client_command":["cli"],"client_procs":1,
  "total_conns":3,"warmup":"1s","duration":"2s","drain":"500ms",
  "scenario":{"name":"env-baseline","kind":"environment_baseline",
    "client_input":{"traffic_id":1,
      "loss_tolerant":{"rate_hz":50,"payload_bytes":64,"staleness_p99_ns":100000000,"min_delivery_ratio":0.9},
      "must_deliver":{"rate_hz":0,"payload_bytes":0}}},
  "drift":{"max_delivery_delta":0.01,"max_staleness_p99_ratio":1.1}
}`

func writeSweepConfig(t *testing.T, body string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "sweep.json")
	if err := os.WriteFile(path, []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}
	return path
}

func TestLoadConfigPilotAcceptsBaseline(t *testing.T) {
	path := writeSweepConfig(t, `{
  "measurement_mode":"pilot","regime":"local",
  "transports":{"t":{"server_command":["s"],"client_command":["c"],"client_procs":1}},
  "workloads":["echo"],"conns":{"min":1,"max":1},"output_dir":"out",`+baselineJSON+`}`)
	cfg, err := LoadConfig(path)
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Baseline == nil || cfg.Baseline.Transport != "raw_udp" ||
		cfg.Baseline.Drift.MaxDeliveryDelta != 0.01 {
		t.Fatalf("baseline = %+v", cfg.Baseline)
	}
}

func TestLoadConfigRejectsBadBaseline(t *testing.T) {
	for name, mutated := range map[string]string{
		"wrong-kind": strings.Replace(baselineJSON, "environment_baseline", "room_relay", 1),
		"zero-delta": strings.Replace(baselineJSON, `"max_delivery_delta":0.01`, `"max_delivery_delta":0`, 1),
		"low-ratio":  strings.Replace(baselineJSON, `"max_staleness_p99_ratio":1.1`, `"max_staleness_p99_ratio":0.5`, 1),
		"no-conns":   strings.Replace(baselineJSON, `"total_conns":3`, `"total_conns":0`, 1),
	} {
		path := writeSweepConfig(t, `{
  "measurement_mode":"pilot","regime":"local",
  "transports":{"t":{"server_command":["s"],"client_command":["c"],"client_procs":1}},
  "workloads":["echo"],"conns":{"min":1,"max":1},"output_dir":"out",`+mutated+`}`)
		if _, err := LoadConfig(path); err == nil || !strings.Contains(err.Error(), "baseline") {
			t.Fatalf("%s: err = %v", name, err)
		}
	}
}

func TestLoadConfigReferenceRequiresBaseline(t *testing.T) {
	path := writeSweepConfig(t, `{
  "measurement_mode":"reference","regime":"local","doctor_report":"missing.json",
  "transports":{"t":{"server_command":["s"],"client_command":["c"],"client_procs":1}},
  "workloads":["echo"],"conns":{"min":1,"max":1},"output_dir":"out"}`)
	if _, err := LoadConfig(path); err == nil || !strings.Contains(err.Error(), "baseline") {
		t.Fatalf("reference without baseline: err = %v", err)
	}
}
