package run

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

type advertisedTreatment struct {
	Transport       string                       `json:"transport"`
	ClassMapping    map[string]ClassMappingSpec  `json:"class_mapping"`
	Coalescing      string                       `json:"coalescing"`
	CCAlgo          string                       `json:"cc_algo"`
	ThreadModel     string                       `json:"thread_model"`
	Encryption      bool                         `json:"encryption"`
	PayloadPattern  string                       `json:"payload_pattern"`
	WireCompression string                       `json:"wire_compression"`
	MaxPayloadBytes int                          `json:"max_payload_bytes"`
	Scenarios       []string                     `json:"scenarios"`
	Tuning          []map[string]json.RawMessage `json:"tuning"`
}

const (
	payloadPatternSplitMix64V1 = "splitmix64-v1"
	wireCompressionNone        = "none"
)

type treatmentValidation struct {
	Invalid     []string
	Unsupported []string
}

func collectTreatment(ctx context.Context, cfg RunConfig) TreatmentRecord {
	record := TreatmentRecord{
		OrchestratorSHA256: OrchestratorFingerprint(),
		EnvironmentSHA256:  EnvironmentFingerprint(),
		Environment:        RelevantEnvironment(),
		Host:               HostEnvironmentSnapshot(),
		Server:             describeCommand(ctx, cfg.ServerCommand),
		Client:             describeCommand(ctx, cfg.ClientCommand),
	}
	record.ClassMapping = collectClassMappingRecord(record.Server, record.Client)
	return record
}

func collectClassMappingRecord(server, client CommandDescription) ClassMappingRecord {
	record := ClassMappingRecord{}
	serverMapping, serverOK := describedClassMapping(server.Description)
	if serverOK {
		record.Server = serverMapping
		record.ServerSHA256 = HashValue(serverMapping)
	}
	clientMapping, clientOK := describedClassMapping(client.Description)
	if clientOK {
		record.Client = clientMapping
		record.ClientSHA256 = HashValue(clientMapping)
	}
	if serverOK && clientOK {
		serverCanonical, _ := json.Marshal(serverMapping)
		clientCanonical, _ := json.Marshal(clientMapping)
		record.Match = bytes.Equal(serverCanonical, clientCanonical)
	}
	return record
}

func describedClassMapping(description json.RawMessage) (map[string]ClassMappingSpec, bool) {
	if len(description) == 0 {
		return nil, false
	}
	var object map[string]json.RawMessage
	if err := json.Unmarshal(description, &object); err != nil {
		return nil, false
	}
	raw, ok := object["class_mapping"]
	if !ok {
		return nil, false
	}
	var mapping map[string]ClassMappingSpec
	if err := json.Unmarshal(raw, &mapping); err != nil || mapping == nil {
		return nil, false
	}
	return mapping, true
}

func describeCommand(ctx context.Context, command CommandConfig) CommandDescription {
	record := CommandDescription{SHA256: CommandFingerprint(command)}
	path := resolveCommandPath(command)
	if path == "" {
		record.Error = fmt.Sprintf("resolve executable %q", command.Path)
		return record
	}
	absolute, err := filepath.Abs(path)
	if err != nil {
		record.Error = err.Error()
		return record
	}
	record.ResolvedPath = absolute
	describeCtx, cancel := context.WithTimeout(ctx, 5*time.Second)
	defer cancel()
	cmd := exec.CommandContext(describeCtx, absolute, "--describe")
	cmd.Dir = command.Dir
	cmd.Env = append(os.Environ(), command.Env...)
	var stdout, stderr bytes.Buffer
	cmd.Stdout, cmd.Stderr = &stdout, &stderr
	if err := cmd.Run(); err != nil {
		record.Error = err.Error()
		if detail := strings.TrimSpace(stderr.String()); detail != "" {
			record.Error += ": " + detail
		}
		return record
	}
	data := bytes.TrimSpace(stdout.Bytes())
	var object map[string]json.RawMessage
	if err := json.Unmarshal(data, &object); err != nil || object == nil {
		if err == nil {
			err = fmt.Errorf("output is not a JSON object")
		}
		record.Error = err.Error()
		return record
	}
	record.Description = append(json.RawMessage(nil), data...)
	return record
}

func classifyScenarioTreatment(record TreatmentRecord, cfg RunConfig) treatmentValidation {
	validation := treatmentValidation{
		Invalid: ValidateTreatmentClassMappingEvidence(&record, cfg.Transport),
	}
	for _, item := range []struct {
		role        string
		description CommandDescription
	}{
		{"server", record.Server},
		{"client", record.Client},
	} {
		if item.description.Error != "" {
			validation.Invalid = append(validation.Invalid, fmt.Sprintf("%s --describe: %s", item.role, item.description.Error))
			continue
		}
		context := item.role + " --describe"
		_, err := requireJSONObject(item.description.Description, context,
			"transport", "class_mapping", "coalescing", "cc_algo", "thread_model", "encryption",
			"payload_pattern", "wire_compression", "max_payload_bytes", "scenarios", "tuning")
		if err != nil {
			validation.Invalid = append(validation.Invalid, err.Error())
			continue
		}
		var advertised advertisedTreatment
		if err := json.Unmarshal(item.description.Description, &advertised); err != nil {
			validation.Invalid = append(validation.Invalid, fmt.Sprintf("%s: %v", context, err))
			continue
		}
		if advertised.Transport != cfg.Transport {
			validation.Invalid = append(validation.Invalid, fmt.Sprintf("%s transport=%q, want %q", context, advertised.Transport, cfg.Transport))
		}
		if advertised.PayloadPattern != payloadPatternSplitMix64V1 {
			validation.Invalid = append(validation.Invalid, fmt.Sprintf("%s payload_pattern=%q, want %q", context, advertised.PayloadPattern, payloadPatternSplitMix64V1))
		}
		if advertised.WireCompression != wireCompressionNone {
			validation.Invalid = append(validation.Invalid, fmt.Sprintf("%s wire_compression=%q, want %q", context, advertised.WireCompression, wireCompressionNone))
		}
		for name, value := range map[string]string{
			"coalescing":   advertised.Coalescing,
			"cc_algo":      advertised.CCAlgo,
			"thread_model": advertised.ThreadModel,
		} {
			if strings.TrimSpace(value) == "" {
				validation.Invalid = append(validation.Invalid, fmt.Sprintf("%s %s is empty", context, name))
			}
		}
		if cfg.Scenario != nil {
			if !containsString(advertised.Scenarios, string(cfg.Scenario.Kind)) {
				validation.Unsupported = append(validation.Unsupported, fmt.Sprintf("%s does not advertise scenario %q", context, cfg.Scenario.Kind))
			}
			if need := scenarioMaxPayload(*cfg.Scenario); advertised.MaxPayloadBytes < need {
				validation.Unsupported = append(validation.Unsupported, fmt.Sprintf("%s max_payload_bytes=%d below scenario payload=%d", context, advertised.MaxPayloadBytes, need))
			}
		}
		for i, tuning := range advertised.Tuning {
			for _, field := range []string{"knob", "value", "upstream_ref"} {
				value, ok := tuning[field]
				var text string
				if !ok || json.Unmarshal(value, &text) != nil || strings.TrimSpace(text) == "" {
					validation.Invalid = append(validation.Invalid, fmt.Sprintf("%s tuning[%d] missing %s", context, i, field))
				}
			}
		}
	}
	return validation
}

func validateClassMapping(data json.RawMessage, context string) (map[string]ClassMappingSpec, []string) {
	var reasons []string
	var entries map[string]json.RawMessage
	if err := json.Unmarshal(data, &entries); err != nil || entries == nil {
		if err == nil {
			err = fmt.Errorf("value is not an object")
		}
		return nil, []string{fmt.Sprintf("%s class_mapping: %v", context, err)}
	}
	for name := range entries {
		if name != ClassLossTolerant && name != ClassMustDeliver {
			reasons = append(reasons, fmt.Sprintf("%s class_mapping has unknown class %q", context, name))
		}
	}
	mapping := make(map[string]ClassMappingSpec, len(metricClassNames))
	for _, class := range metricClassNames {
		raw, ok := entries[class]
		if !ok || string(raw) == "null" {
			reasons = append(reasons, fmt.Sprintf("%s missing class_mapping.%s", context, class))
			continue
		}
		fields, err := requireJSONObject(raw, context+" class_mapping."+class,
			"primitive", "delivery", "ordering", "realization")
		if err != nil {
			reasons = append(reasons, err.Error())
			continue
		}
		for name := range fields {
			switch name {
			case "primitive", "delivery", "ordering", "realization":
			default:
				reasons = append(reasons, fmt.Sprintf("%s class_mapping.%s has unknown field %q", context, class, name))
			}
		}
		var spec ClassMappingSpec
		if err := json.Unmarshal(raw, &spec); err != nil {
			reasons = append(reasons, fmt.Sprintf("%s class_mapping.%s: %v", context, class, err))
			continue
		}
		mapping[class] = spec
		reasons = append(reasons, validateClassMappingSpec(context, class, spec)...)
	}
	sort.Strings(reasons)
	return mapping, reasons
}

func validateClassMappingSpec(context, class string, spec ClassMappingSpec) []string {
	var reasons []string
	prefix := context + " class_mapping." + class
	if strings.TrimSpace(spec.Primitive) == "" {
		reasons = append(reasons, prefix+" primitive is empty")
	}
	if spec.Delivery != ClassMappingDeliveryBestEffort && spec.Delivery != ClassMappingDeliveryReliable {
		reasons = append(reasons, fmt.Sprintf("%s delivery=%q is invalid", prefix, spec.Delivery))
	}
	if spec.Ordering != ClassMappingOrderingUnordered && spec.Ordering != ClassMappingOrderingOrdered {
		reasons = append(reasons, fmt.Sprintf("%s ordering=%q is invalid", prefix, spec.Ordering))
	}
	switch spec.Realization {
	case ClassMappingRealizationNative, ClassMappingRealizationEmulated:
		if class == ClassLossTolerant && spec.Delivery != ClassMappingDeliveryBestEffort {
			reasons = append(reasons, fmt.Sprintf("%s realization=%q requires delivery=%q", prefix, spec.Realization, ClassMappingDeliveryBestEffort))
		}
		if class == ClassMustDeliver &&
			(spec.Delivery != ClassMappingDeliveryReliable || spec.Ordering != ClassMappingOrderingOrdered) {
			reasons = append(reasons, fmt.Sprintf("%s realization=%q requires delivery=%q and ordering=%q",
				prefix, spec.Realization, ClassMappingDeliveryReliable, ClassMappingOrderingOrdered))
		}
	case ClassMappingRealizationReliableFallback:
		if class != ClassLossTolerant || spec.Delivery != ClassMappingDeliveryReliable {
			reasons = append(reasons, fmt.Sprintf("%s realization=%q requires loss_tolerant with delivery=%q", prefix, spec.Realization, ClassMappingDeliveryReliable))
		}
	case ClassMappingRealizationUnsupported:
		providesRequestedGuarantee := class == ClassLossTolerant && spec.Delivery == ClassMappingDeliveryBestEffort ||
			class == ClassMustDeliver && spec.Delivery == ClassMappingDeliveryReliable && spec.Ordering == ClassMappingOrderingOrdered
		if providesRequestedGuarantee {
			reasons = append(reasons, fmt.Sprintf("%s realization=%q must disclose an absent class delivery guarantee", prefix, spec.Realization))
		}
	default:
		reasons = append(reasons, fmt.Sprintf("%s realization=%q is invalid", prefix, spec.Realization))
	}
	return reasons
}

func canonicalClassMappingsEqual(a, b map[string]ClassMappingSpec) bool {
	aJSON, err := json.Marshal(a)
	if err != nil {
		return false
	}
	bJSON, err := json.Marshal(b)
	return err == nil && bytes.Equal(aJSON, bJSON)
}

// ValidateScenarioTreatmentContract applies the current --describe schema and
// verifies that the structured evidence in TreatmentRecord is an exact
// canonical rendering of the two endpoint descriptions.
func ValidateScenarioTreatmentContract(record *TreatmentRecord, cfg RunConfig) (invalid, unsupported []string) {
	if record == nil {
		return []string{"treatment record is missing"}, nil
	}
	validation := classifyScenarioTreatment(*record, cfg)
	invalid = append(invalid, validation.Invalid...)
	unsupported = append(unsupported, validation.Unsupported...)
	if cfg.ClassMappingSHA256 != "" &&
		(record.ClassMapping.ServerSHA256 != cfg.ClassMappingSHA256 || record.ClassMapping.ClientSHA256 != cfg.ClassMappingSHA256) {
		invalid = append(invalid, fmt.Sprintf("class_mapping_sha256 drift: server=%q client=%q, expected %q",
			record.ClassMapping.ServerSHA256, record.ClassMapping.ClientSHA256, cfg.ClassMappingSHA256))
	}
	return invalid, unsupported
}

// PreflightClassMapping executes each endpoint's --describe once and returns a
// validated canonical mapping record suitable for binding into sweep identity.
func PreflightClassMapping(ctx context.Context, transport string, server, client CommandConfig) (ClassMappingRecord, error) {
	treatment := &TreatmentRecord{
		Server: describeCommand(ctx, server),
		Client: describeCommand(ctx, client),
	}
	treatment.ClassMapping = collectClassMappingRecord(treatment.Server, treatment.Client)
	if reasons := ValidateTreatmentClassMappingEvidence(treatment, transport); len(reasons) > 0 {
		return treatment.ClassMapping, fmt.Errorf("%s", strings.Join(reasons, "; "))
	}
	return treatment.ClassMapping, nil
}

// ValidateTreatmentClassMappingEvidence validates endpoint descriptions,
// strict mapping schema, canonical agreement, stored hashes, and the exact
// structured rendering without executing either endpoint.
func ValidateTreatmentClassMappingEvidence(record *TreatmentRecord, transport string) []string {
	if record == nil {
		return []string{"treatment record is missing"}
	}
	var reasons []string
	validMappings := map[string]map[string]ClassMappingSpec{}
	for _, endpoint := range []struct {
		name        string
		description CommandDescription
	}{
		{"server", record.Server},
		{"client", record.Client},
	} {
		if endpoint.description.Error != "" {
			reasons = append(reasons, fmt.Sprintf("%s --describe: %s", endpoint.name, endpoint.description.Error))
			continue
		}
		context := endpoint.name + " --describe"
		object, err := requireJSONObject(endpoint.description.Description, context, "transport", "class_mapping")
		if err != nil {
			reasons = append(reasons, err.Error())
			continue
		}
		var advertisedTransport string
		if err := json.Unmarshal(object["transport"], &advertisedTransport); err != nil {
			reasons = append(reasons, fmt.Sprintf("%s transport: %v", context, err))
		} else if advertisedTransport != transport {
			reasons = append(reasons, fmt.Sprintf("%s transport=%q, want %q", context, advertisedTransport, transport))
		}
		mapping, mappingReasons := validateClassMapping(object["class_mapping"], context)
		reasons = append(reasons, mappingReasons...)
		if len(mappingReasons) == 0 {
			validMappings[endpoint.name] = mapping
		}
	}
	server, serverOK := validMappings["server"]
	client, clientOK := validMappings["client"]
	if serverOK && clientOK && !canonicalClassMappingsEqual(server, client) {
		reasons = append(reasons, "server/client --describe class_mapping canonical mismatch")
	}
	reasons = append(reasons, validateStructuredClassMappingEvidence(record)...)
	sort.Strings(reasons)
	return reasons
}

func validateStructuredClassMappingEvidence(record *TreatmentRecord) []string {
	reasons := ValidateClassMappingRecord(record.ClassMapping)
	expected := collectClassMappingRecord(record.Server, record.Client)
	if HashValue(record.ClassMapping) != HashValue(expected) {
		reasons = append(reasons, "structured class_mapping evidence does not match endpoint --describe output")
	}
	return reasons
}

// ValidateClassMappingRecord verifies the structured mapping schema, its
// canonical hashes, and endpoint agreement. It is also used by report readers
// so malformed persisted evidence is never rendered as a valid mapping.
func ValidateClassMappingRecord(record ClassMappingRecord) []string {
	var reasons []string
	for _, endpoint := range []struct {
		name    string
		mapping map[string]ClassMappingSpec
		hash    string
	}{
		{"server", record.Server, record.ServerSHA256},
		{"client", record.Client, record.ClientSHA256},
	} {
		if endpoint.mapping == nil {
			reasons = append(reasons, endpoint.name+" structured class_mapping is missing")
			continue
		}
		raw, _ := json.Marshal(endpoint.mapping)
		_, mappingReasons := validateClassMapping(raw, endpoint.name+" treatment record")
		reasons = append(reasons, mappingReasons...)
		wantHash := HashValue(endpoint.mapping)
		if endpoint.hash != wantHash {
			reasons = append(reasons, fmt.Sprintf("%s class_mapping_sha256=%q, want %q", endpoint.name, endpoint.hash, wantHash))
		}
	}
	actualMatch := record.Server != nil && record.Client != nil && canonicalClassMappingsEqual(record.Server, record.Client)
	if record.Match != actualMatch {
		reasons = append(reasons, fmt.Sprintf("class_mapping match=%v, want %v", record.Match, actualMatch))
	}
	if !actualMatch {
		reasons = append(reasons, "structured server/client class_mapping canonical mismatch")
	}
	sort.Strings(reasons)
	return reasons
}

func validateScenarioTreatment(record TreatmentRecord, cfg RunConfig) []string {
	invalid, unsupported := ValidateScenarioTreatmentContract(&record, cfg)
	return append(invalid, unsupported...)
}

func scenarioMaxPayload(scenario ScenarioSpec) int {
	maxPayload := 0
	for _, trafficCase := range scenarioMetricCases(scenario) {
		if trafficCase.spec == nil {
			continue
		}
		for _, class := range []TrafficClassSpec{trafficCase.spec.LossTolerant, trafficCase.spec.MustDeliver} {
			if class.RateHz > 0 && class.PayloadBytes > maxPayload {
				maxPayload = class.PayloadBytes
			}
		}
	}
	return maxPayload
}

func containsString(values []string, want string) bool {
	for _, value := range values {
		if value == want {
			return true
		}
	}
	return false
}
