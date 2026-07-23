package sweep

import (
	"strings"
	"testing"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/run"
)

const presetSweepTransports = `"transports":{"enet":{"server_command":["s"],"client_command":["c"],"client_procs":1}}`

func TestLoadConfigPresetExpandsFrozenValues(t *testing.T) {
	path := writeSweepConfig(t, `{
  "regime":"ref-wan","preset":"ref-auth-wan-s1000-v1",
  "server_cpus":"7,15",`+presetSweepTransports+`,
  "conns":{"min":1,"max":8},"output_dir":"out"}`)
	cfg, err := LoadConfig(path)
	if err != nil {
		t.Fatal(err)
	}
	if len(cfg.Scenarios) != 1 || cfg.Scenarios[0].Name != "ref-auth-wan-s1000-v1" {
		t.Fatalf("scenarios = %+v", cfg.Scenarios)
	}
	if cfg.Warmup.Duration != 25*time.Second || cfg.Duration.Duration != 60*time.Second ||
		cfg.Drain.Duration != 5*time.Second || !cfg.SteadyWarmup {
		t.Fatalf("frozen durations not applied: %+v", cfg)
	}
	if cfg.Netem == nil || cfg.Netem.ClientEgress.DelayMS != 25 || cfg.Netem.ClientEgress.LossPercent != 1 ||
		cfg.Netem.LinkMTUBytes != 1500 || !cfg.Netem.DisableOffloads {
		t.Fatalf("netem = %+v", cfg.Netem)
	}
	if cfg.StalenessPeriodNS == 0 {
		t.Fatalf("staleness period not applied")
	}
}

func TestLoadConfigPresetRejectsOverrides(t *testing.T) {
	for field, body := range map[string]string{
		"workloads":           `"workloads":["echo"]`,
		"scenarios":           `"scenarios":[{"name":"x","kind":"environment_baseline","client_input":{"traffic_id":1,"loss_tolerant":{"rate_hz":1,"payload_bytes":32,"min_delivery_ratio":0.9},"must_deliver":{"rate_hz":0,"payload_bytes":0}}}]`,
		"netem":               `"netem":{"link_mtu_bytes":1500}`,
		"warmup":              `"warmup":"1s"`,
		"duration":            `"duration":"2s"`,
		"drain":               `"drain":"1s"`,
		"staleness_period_ns": `"staleness_period_ns":1`,
		"steady_warmup":       `"steady_warmup":true`,
	} {
		path := writeSweepConfig(t, `{
  "regime":"ref-wan","preset":"ref-auth-wan-s1000-v1",
  "server_cpus":"7,15",`+presetSweepTransports+`,`+body+`,
  "conns":{"min":1,"max":8},"output_dir":"out"}`)
		if _, err := LoadConfig(path); err == nil || !strings.Contains(err.Error(), "preset") {
			t.Fatalf("%s: override accepted (err=%v)", field, err)
		}
	}
}

// budget 検査は LoadConfig でなく New の入口(validatePresetBudget)で行う:
// block 実行では server_cpus が LoadConfig 後に rig から注入されるため。
func TestPresetBudgetValidation(t *testing.T) {
	path := writeSweepConfig(t, `{
  "regime":"ref-wan","preset":"ref-auth-wan-s1000-v1",
  `+presetSweepTransports+`,
  "conns":{"min":1,"max":8},"output_dir":"out"}`)
	cfg, err := LoadConfig(path)
	if err != nil {
		t.Fatalf("server_cpus 未注入の load が失敗: %v", err)
	}
	for name, cpus := range map[string]string{"one": "7", "none": ""} {
		cfg.ServerCPUs = cpus
		if err := cfg.validatePresetBudget(); err == nil || !strings.Contains(err.Error(), "server_cpus") {
			t.Fatalf("%s: bad budget accepted (err=%v)", name, err)
		}
	}
	cfg.ServerCPUs = "7,15"
	if err := cfg.validatePresetBudget(); err != nil {
		t.Fatalf("2-CPU budget rejected: %v", err)
	}
}

func TestLoadConfigPresetUnknownName(t *testing.T) {
	path := writeSweepConfig(t, `{
  "regime":"ref-wan","preset":"ref-nope-v1","server_cpus":"7,15",
  `+presetSweepTransports+`,
  "conns":{"min":1,"max":8},"output_dir":"out"}`)
	if _, err := LoadConfig(path); err == nil || !strings.Contains(err.Error(), "preset") {
		t.Fatalf("unknown preset accepted (err=%v)", err)
	}
}

func TestLoadConfigReferenceFreezesDriftTolerances(t *testing.T) {
	frozen := run.ConfirmatoryV1()
	for name, drift := range map[string]string{
		"loose-delivery":  `"drift":{"max_delivery_delta":0.05,"max_staleness_p99_ratio":1.1}`,
		"loose-staleness": `"drift":{"max_delivery_delta":0.01,"max_staleness_p99_ratio":1.5}`,
	} {
		body := strings.Replace(baselineJSON,
			`"drift":{"max_delivery_delta":0.01,"max_staleness_p99_ratio":1.1}`, drift, 1)
		if body == baselineJSON {
			t.Fatal("baselineJSON drift stanza changed; update the test")
		}
		path := writeSweepConfig(t, `{
  "measurement_mode":"reference","regime":"local","doctor_report":"missing.json",
  "transports":{"t":{"server_command":["s"],"client_command":["c"],"client_procs":1}},
  "workloads":["echo"],"conns":{"min":4,"max":8},"output_dir":"out",`+body+`}`)
		if _, err := LoadConfig(path); err == nil || !strings.Contains(err.Error(), "frozen") {
			t.Fatalf("%s: unfrozen drift accepted (err=%v)", name, err)
		}
	}
	// 凍結値そのもの(0.010 / 1.10 倍)は drift では弾かれず、次の gate
	// (doctor_report)まで進む
	path := writeSweepConfig(t, `{
  "measurement_mode":"reference","regime":"local","doctor_report":"missing.json",
  "transports":{"t":{"server_command":["s"],"client_command":["c"],"client_procs":1}},
  "workloads":["echo"],"conns":{"min":4,"max":8},"output_dir":"out",`+baselineJSON+`}`)
	if _, err := LoadConfig(path); err == nil || !strings.Contains(err.Error(), "doctor_report") {
		t.Fatalf("frozen drift (%.3f/%.2f) rejected or doctor gate skipped (err=%v)",
			frozen.DriftMaxDeliveryDelta, frozen.DriftMaxStalenessP99Ratio, err)
	}
}
