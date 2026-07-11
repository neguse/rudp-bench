package run

import (
	"encoding/json"
	"strings"
	"testing"
)

func TestValidateScenarioTreatment(t *testing.T) {
	description := json.RawMessage(`{
  "transport":"fake",
  "class_mapping":{"loss_tolerant":"unreliable","must_deliver":"reliable"},
  "coalescing":"none","cc_algo":"none","thread_model":"single","encryption":false,
  "max_payload_bytes":1024,
  "scenarios":["environment_baseline","authoritative_state","room_relay"],
  "tuning":[{"knob":"buffer","value":"1MB","upstream_ref":"https://example.invalid/upstream"}]
}`)
	cfg := RunConfig{Transport: "fake", Scenario: ptr(authoritativeFixture())}
	record := TreatmentRecord{
		Server: CommandDescription{Description: description},
		Client: CommandDescription{Description: description},
	}
	if reasons := validateScenarioTreatment(record, cfg); len(reasons) != 0 {
		t.Fatalf("reasons = %v", reasons)
	}

	bad := append(json.RawMessage(nil), description...)
	bad = json.RawMessage(strings.Replace(string(bad), `"max_payload_bytes":1024`, `"max_payload_bytes":32`, 1))
	record.Client.Description = bad
	reasons := validateScenarioTreatment(record, cfg)
	if len(reasons) == 0 || !strings.Contains(strings.Join(reasons, "; "), "below scenario payload") {
		t.Fatalf("reasons = %v", reasons)
	}
}

func TestValidateScenarioTreatmentRejectsStringTuning(t *testing.T) {
	description := json.RawMessage(`{
  "transport":"fake",
  "class_mapping":{"loss_tolerant":"unreliable","must_deliver":"reliable"},
  "coalescing":"none","cc_algo":"none","thread_model":"single","encryption":false,
  "max_payload_bytes":1024,"scenarios":["authoritative_state"],"tuning":["opaque"]
}`)
	cfg := RunConfig{Transport: "fake", Scenario: ptr(authoritativeFixture())}
	record := TreatmentRecord{
		Server: CommandDescription{Description: description},
		Client: CommandDescription{Description: description},
	}
	if reasons := validateScenarioTreatment(record, cfg); len(reasons) == 0 {
		t.Fatal("string tuning unexpectedly accepted")
	}
}
