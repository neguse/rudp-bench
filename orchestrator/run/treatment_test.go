package run

import (
	"encoding/json"
	"strings"
	"testing"
)

const validClassMapping = `{
  "loss_tolerant":{"primitive":"unreliable","delivery":"best_effort","ordering":"unordered","realization":"native"},
  "must_deliver":{"primitive":"reliable","delivery":"reliable","ordering":"ordered","realization":"native"}
}`

func treatmentDescription(mapping string) json.RawMessage {
	return json.RawMessage(`{
  "transport":"fake",
  "class_mapping":` + mapping + `,
  "coalescing":"none","cc_algo":"none","thread_model":"single","encryption":false,
  "payload_pattern":"splitmix64-v1","wire_compression":"none",
  "max_payload_bytes":1024,
  "scenarios":["environment_baseline","authoritative_state","room_relay"],
  "tuning":[{"knob":"buffer","value":"1MB","upstream_ref":"https://example.invalid/upstream"}]
}`)
}

func TestValidateScenarioTreatmentRejectsPayloadOrCompressionDrift(t *testing.T) {
	description := treatmentDescription(validClassMapping)
	tests := map[string]string{
		"missing payload pattern": strings.Replace(string(description), `  "payload_pattern":"splitmix64-v1",`, "", 1),
		"wrong payload pattern":   strings.Replace(string(description), `"payload_pattern":"splitmix64-v1"`, `"payload_pattern":"periodic"`, 1),
		"missing compression":     strings.Replace(string(description), `"wire_compression":"none",`, "", 1),
		"enabled compression":     strings.Replace(string(description), `"wire_compression":"none"`, `"wire_compression":"deflate"`, 1),
	}
	for name, raw := range tests {
		t.Run(name, func(t *testing.T) {
			drifted := json.RawMessage(raw)
			record, cfg := treatmentFixture(drifted, drifted)
			if reasons := validateScenarioTreatment(record, cfg); len(reasons) == 0 {
				t.Fatal("drifted payload contract unexpectedly accepted")
			}
		})
	}
}

func treatmentFixture(server, client json.RawMessage) (TreatmentRecord, RunConfig) {
	record := TreatmentRecord{
		Server: CommandDescription{Description: server},
		Client: CommandDescription{Description: client},
	}
	record.ClassMapping = collectClassMappingRecord(record.Server, record.Client)
	return record, RunConfig{Transport: "fake", Scenario: ptr(authoritativeFixture())}
}

func TestValidateScenarioTreatment(t *testing.T) {
	description := treatmentDescription(validClassMapping)
	record, cfg := treatmentFixture(description, description)
	if reasons := validateScenarioTreatment(record, cfg); len(reasons) != 0 {
		t.Fatalf("reasons = %v", reasons)
	}

	bad := json.RawMessage(strings.Replace(string(description), `"max_payload_bytes":1024`, `"max_payload_bytes":32`, 1))
	record.Client.Description = bad
	reasons := validateScenarioTreatment(record, cfg)
	if len(reasons) == 0 || !strings.Contains(strings.Join(reasons, "; "), "below scenario payload") {
		t.Fatalf("reasons = %v", reasons)
	}
}

func TestClassMappingRecordUsesCanonicalHashes(t *testing.T) {
	server := treatmentDescription(validClassMapping)
	client := treatmentDescription(`{
  "must_deliver":{"realization":"native","ordering":"ordered","delivery":"reliable","primitive":"reliable"},
  "loss_tolerant":{"ordering":"unordered","primitive":"unreliable","realization":"native","delivery":"best_effort"}
}`)
	record := collectClassMappingRecord(
		CommandDescription{Description: server},
		CommandDescription{Description: client},
	)
	if !record.Match || record.ServerSHA256 == "" || record.ServerSHA256 != record.ClientSHA256 {
		t.Fatalf("record = %+v", record)
	}
	if record.Server[ClassLossTolerant].Primitive != "unreliable" || record.Client[ClassMustDeliver].Primitive != "reliable" {
		t.Fatalf("record mappings = server:%+v client:%+v", record.Server, record.Client)
	}
}

func TestValidateScenarioTreatmentContractRejectsTamperedMappingEvidence(t *testing.T) {
	description := treatmentDescription(validClassMapping)
	record, cfg := treatmentFixture(description, description)
	record.ClassMapping.ServerSHA256 = "tampered"
	invalid, unsupported := ValidateScenarioTreatmentContract(&record, cfg)
	if len(unsupported) != 0 {
		t.Fatalf("unsupported = %v", unsupported)
	}
	joined := strings.Join(invalid, "; ")
	if !strings.Contains(joined, "server class_mapping_sha256") ||
		!strings.Contains(joined, "does not match endpoint --describe") {
		t.Fatalf("invalid = %v", invalid)
	}
}

func TestValidateTreatmentContractRejectsMappingDrift(t *testing.T) {
	description := treatmentDescription(validClassMapping)
	record, cfg := treatmentFixture(description, description)
	cfg.ClassMappingSHA256 = strings.Repeat("f", 64)
	invalid, _ := ValidateScenarioTreatmentContract(&record, cfg)
	if got := strings.Join(invalid, "; "); !strings.Contains(got, "class_mapping_sha256 drift") {
		t.Fatalf("invalid = %v", invalid)
	}
}

func TestValidateScenarioTreatmentRejectsEndpointMappingMismatch(t *testing.T) {
	server := treatmentDescription(validClassMapping)
	client := json.RawMessage(strings.Replace(string(server), `"primitive":"reliable"`, `"primitive":"other-reliable"`, 1))
	record, cfg := treatmentFixture(server, client)
	validation := classifyScenarioTreatment(record, cfg)
	if got := strings.Join(validation.Invalid, "; "); !strings.Contains(got, "canonical mismatch") {
		t.Fatalf("invalid reasons = %v", validation.Invalid)
	}
	mappingRecord := collectClassMappingRecord(record.Server, record.Client)
	if mappingRecord.Match || mappingRecord.ServerSHA256 == mappingRecord.ClientSHA256 {
		t.Fatalf("mapping record = %+v", mappingRecord)
	}
}

func TestValidateClassMappingEnumsAndCombinations(t *testing.T) {
	tests := map[string]string{
		"missing class": strings.Replace(validClassMapping,
			`,
  "must_deliver":{"primitive":"reliable","delivery":"reliable","ordering":"ordered","realization":"native"}`, "", 1),
		"empty primitive":               strings.Replace(validClassMapping, `"primitive":"unreliable"`, `"primitive":" "`, 1),
		"unknown delivery":              strings.Replace(validClassMapping, `"delivery":"best_effort"`, `"delivery":"maybe"`, 1),
		"native must deliver unordered": strings.Replace(validClassMapping, `"ordering":"ordered"`, `"ordering":"unordered"`, 1),
		"emulated must deliver unordered": strings.Replace(validClassMapping,
			`"must_deliver":{"primitive":"reliable","delivery":"reliable","ordering":"ordered","realization":"native"}`,
			`"must_deliver":{"primitive":"reliable","delivery":"reliable","ordering":"unordered","realization":"emulated"}`, 1),
		"fallback on must deliver": strings.Replace(validClassMapping,
			`"must_deliver":{"primitive":"reliable","delivery":"reliable","ordering":"ordered","realization":"native"}`,
			`"must_deliver":{"primitive":"reliable","delivery":"reliable","ordering":"ordered","realization":"reliable_fallback"}`, 1),
		"unsupported despite guarantee": strings.Replace(validClassMapping,
			`"must_deliver":{"primitive":"reliable","delivery":"reliable","ordering":"ordered","realization":"native"}`,
			`"must_deliver":{"primitive":"reliable","delivery":"reliable","ordering":"ordered","realization":"unsupported"}`, 1),
		"unknown class": strings.TrimSuffix(validClassMapping, "}") + `,"extra":{"primitive":"x","delivery":"best_effort","ordering":"unordered","realization":"native"}}`,
		"unknown field": strings.Replace(validClassMapping, `"realization":"native"`, `"realization":"native","extra":true`, 1),
	}
	for name, mapping := range tests {
		t.Run(name, func(t *testing.T) {
			description := treatmentDescription(mapping)
			record, cfg := treatmentFixture(description, description)
			if reasons := validateScenarioTreatment(record, cfg); len(reasons) == 0 {
				t.Fatalf("mapping unexpectedly accepted: %s", mapping)
			}
		})
	}
}

func TestUnsupportedMustDeliverAllowsIncompleteGuarantee(t *testing.T) {
	mapping := strings.Replace(validClassMapping,
		`"primitive":"reliable","delivery":"reliable","ordering":"ordered","realization":"native"`,
		`"primitive":"reliable-unordered","delivery":"reliable","ordering":"unordered","realization":"unsupported"`, 1)
	description := treatmentDescription(mapping)
	record, cfg := treatmentFixture(description, description)
	validation := classifyScenarioTreatment(record, cfg)
	if len(validation.Invalid) != 0 || len(validation.Unsupported) != 0 {
		t.Fatalf("incomplete must-deliver guarantee disclosure rejected: %+v", validation)
	}
}

func TestUnsupportedMappingIsDisclosureNotRunOutcome(t *testing.T) {
	mapping := `{
  "loss_tolerant":{"primitive":"unreliable-udp","delivery":"best_effort","ordering":"unordered","realization":"native"},
  "must_deliver":{"primitive":"unreliable-udp","delivery":"best_effort","ordering":"unordered","realization":"unsupported"}
}`
	description := treatmentDescription(mapping)
	record, cfg := treatmentFixture(description, description)
	validation := classifyScenarioTreatment(record, cfg)
	if len(validation.Invalid) != 0 || len(validation.Unsupported) != 0 {
		t.Fatalf("unsupported disclosure changed run classification: %+v", validation)
	}
}

func TestReliableFallbackIsAcceptedForLossTolerant(t *testing.T) {
	mapping := strings.Replace(validClassMapping,
		`"primitive":"unreliable","delivery":"best_effort","ordering":"unordered","realization":"native"`,
		`"primitive":"reliable-stream","delivery":"reliable","ordering":"ordered","realization":"reliable_fallback"`, 1)
	description := treatmentDescription(mapping)
	record, cfg := treatmentFixture(description, description)
	if reasons := validateScenarioTreatment(record, cfg); len(reasons) != 0 {
		t.Fatalf("reliable fallback reasons = %v", reasons)
	}
}

func TestValidateScenarioTreatmentRejectsStringTuning(t *testing.T) {
	description := json.RawMessage(strings.Replace(string(treatmentDescription(validClassMapping)),
		`"tuning":[{"knob":"buffer","value":"1MB","upstream_ref":"https://example.invalid/upstream"}]`,
		`"tuning":["opaque"]`, 1))
	record, cfg := treatmentFixture(description, description)
	if reasons := validateScenarioTreatment(record, cfg); len(reasons) == 0 {
		t.Fatal("string tuning unexpectedly accepted")
	}
}
