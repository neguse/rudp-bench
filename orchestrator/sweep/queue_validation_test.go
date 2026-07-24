package sweep

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// reference campaign(ADR-0006)の queue が計測器の config 契約と噛み合って
// いることの回帰テスト。screening config は load が通ること、reference mode の
// config(confirmatory の job と template)は preset・baseline・凍結 drift の
// 検査を通過して doctor_report の読み込み(ホスト上でのみ成立)まで到達する
// ことを確認する。
func TestReferenceQueueConfigs(t *testing.T) {
	queueRoot := filepath.Join("..", "..", "scripts", "fleet", "queues")
	sweeps, err := filepath.Glob(filepath.Join(queueRoot, "ref1-*", "*", "sweep.json"))
	if err != nil {
		t.Fatal(err)
	}
	if len(sweeps) == 0 {
		t.Skip("no reference queues generated")
	}
	for _, path := range sweeps {
		raw, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		var head struct {
			MeasurementMode string `json:"measurement_mode"`
		}
		if err := json.Unmarshal(raw, &head); err != nil {
			t.Fatalf("%s: %v", path, err)
		}
		switch head.MeasurementMode {
		case "", "screening":
			cfg, err := LoadConfig(path)
			if err != nil {
				t.Fatalf("%s: %v", path, err)
			}
			if len(cfg.Scenarios) != 1 {
				t.Fatalf("%s: scenarios = %d", path, len(cfg.Scenarios))
			}
		case "reference":
			if _, err := LoadConfig(path); err == nil || !strings.Contains(err.Error(), "doctor_report") {
				t.Fatalf("%s: want doctor_report error (baseline and drift gates passed), got %v", path, err)
			}
		default:
			t.Fatalf("%s: unexpected measurement_mode %q in a campaign queue", path, head.MeasurementMode)
		}
	}
	templates, err := filepath.Glob(filepath.Join(queueRoot, "ref1-*", "confirm-*.tmpl.json"))
	if err != nil {
		t.Fatal(err)
	}
	if len(templates) == 0 {
		t.Fatal("screening queues exist but no confirmatory templates")
	}
	for _, path := range templates {
		_, err := LoadConfig(path)
		if err == nil || !strings.Contains(err.Error(), "doctor_report") {
			t.Fatalf("%s: want doctor_report error (baseline and drift gates passed), got %v", path, err)
		}
	}
}
