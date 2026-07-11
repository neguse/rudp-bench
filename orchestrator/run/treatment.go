package run

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"
)

type advertisedTreatment struct {
	Transport       string                       `json:"transport"`
	ClassMapping    map[string]string            `json:"class_mapping"`
	Coalescing      string                       `json:"coalescing"`
	CCAlgo          string                       `json:"cc_algo"`
	ThreadModel     string                       `json:"thread_model"`
	Encryption      bool                         `json:"encryption"`
	MaxPayloadBytes int                          `json:"max_payload_bytes"`
	Scenarios       []string                     `json:"scenarios"`
	Tuning          []map[string]json.RawMessage `json:"tuning"`
}

type treatmentValidation struct {
	Invalid     []string
	Unsupported []string
}

func collectTreatment(ctx context.Context, cfg RunConfig) TreatmentRecord {
	return TreatmentRecord{
		OrchestratorSHA256: OrchestratorFingerprint(),
		EnvironmentSHA256:  EnvironmentFingerprint(),
		Environment:        RelevantEnvironment(),
		Host:               HostEnvironmentSnapshot(),
		Server:             describeCommand(ctx, cfg.ServerCommand),
		Client:             describeCommand(ctx, cfg.ClientCommand),
	}
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
	var validation treatmentValidation
	if cfg.Scenario == nil {
		return validation
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
		if _, err := requireJSONObject(item.description.Description, context,
			"transport", "class_mapping", "coalescing", "cc_algo", "thread_model", "encryption",
			"max_payload_bytes", "scenarios", "tuning"); err != nil {
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
		for name, value := range map[string]string{
			"coalescing":   advertised.Coalescing,
			"cc_algo":      advertised.CCAlgo,
			"thread_model": advertised.ThreadModel,
		} {
			if strings.TrimSpace(value) == "" {
				validation.Invalid = append(validation.Invalid, fmt.Sprintf("%s %s is empty", context, name))
			}
		}
		for _, class := range metricClassNames {
			if advertised.ClassMapping[class] == "" {
				validation.Invalid = append(validation.Invalid, fmt.Sprintf("%s missing class_mapping.%s", context, class))
			}
		}
		if !containsString(advertised.Scenarios, string(cfg.Scenario.Kind)) {
			validation.Unsupported = append(validation.Unsupported, fmt.Sprintf("%s does not advertise scenario %q", context, cfg.Scenario.Kind))
		}
		if need := scenarioMaxPayload(*cfg.Scenario); advertised.MaxPayloadBytes < need {
			validation.Unsupported = append(validation.Unsupported, fmt.Sprintf("%s max_payload_bytes=%d below scenario payload=%d", context, advertised.MaxPayloadBytes, need))
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

func validateScenarioTreatment(record TreatmentRecord, cfg RunConfig) []string {
	validation := classifyScenarioTreatment(record, cfg)
	return append(validation.Invalid, validation.Unsupported...)
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
