package sweep

import (
	"path/filepath"
	"strings"
	"testing"
)

// reference campaign(ADR-0006)の queue が計測器の config 契約と噛み合って
// いることの回帰テスト。screening config は load が通ること、confirmatory
// template は preset・baseline・凍結 drift の検査を通過して doctor_report の
// 読み込み(ホスト上でのみ成立)まで到達することを確認する。
func TestReferenceQueueConfigs(t *testing.T) {
	queueRoot := filepath.Join("..", "..", "scripts", "fleet", "queues")
	screening, err := filepath.Glob(filepath.Join(queueRoot, "ref1-*", "*", "sweep.json"))
	if err != nil {
		t.Fatal(err)
	}
	if len(screening) == 0 {
		t.Skip("no reference queues generated")
	}
	for _, path := range screening {
		cfg, err := LoadConfig(path)
		if err != nil {
			t.Fatalf("%s: %v", path, err)
		}
		if cfg.MeasurementMode != "screening" {
			t.Fatalf("%s: measurement_mode = %q", path, cfg.MeasurementMode)
		}
		if len(cfg.Scenarios) != 1 {
			t.Fatalf("%s: scenarios = %d", path, len(cfg.Scenarios))
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
