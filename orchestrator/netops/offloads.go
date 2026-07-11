package netops

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"maps"
	"os/exec"
	"slices"
	"strings"
)

const OffloadEvidenceVersion = 2

type offloadFeatureRequirement struct {
	Name     string
	SetAlias string
}

// Only features which can split or combine packets are changed. Checksum and
// scatter/gather offloads do not alter the packet exposure counted by netem.
var offloadFeatureRequirements = []offloadFeatureRequirement{
	{Name: "tcp-segmentation-offload", SetAlias: "tso"},
	{Name: "generic-segmentation-offload", SetAlias: "gso"},
	{Name: "generic-receive-offload", SetAlias: "gro"},
	{Name: "large-receive-offload", SetAlias: "lro"},
	{Name: "tx-udp-segmentation", SetAlias: "tx-udp-segmentation"},
	{Name: "rx-udp-gro-forwarding", SetAlias: "rx-udp-gro-forwarding"},
}

type OffloadFeatureState struct {
	Enabled bool `json:"enabled"`
	Fixed   bool `json:"fixed,omitempty"`
}

type OffloadInterfaceEvidence struct {
	Namespace    string                         `json:"namespace"`
	Device       string                         `json:"device"`
	LinkMTUBytes int                            `json:"link_mtu_bytes"`
	Features     map[string]OffloadFeatureState `json:"features,omitempty"`
	Raw          string                         `json:"raw,omitempty"`
	LinkRaw      string                         `json:"link_raw,omitempty"`
	Error        string                         `json:"error,omitempty"`
}

// OffloadEvidence is a setup-time readback from both veth endpoints. SHA256
// covers every other field, including the raw ethtool output and normalized
// feature map.
type OffloadEvidence struct {
	Version          int                      `json:"version"`
	RequiredFeatures []string                 `json:"required_features"`
	Server           OffloadInterfaceEvidence `json:"server"`
	Client           OffloadInterfaceEvidence `json:"client"`
	SHA256           string                   `json:"sha256"`
}

func RequiredOffloadFeatures() []string {
	out := make([]string, 0, len(offloadFeatureRequirements))
	for _, feature := range offloadFeatureRequirements {
		out = append(out, feature.Name)
	}
	return out
}

func buildDisableOffloadsCommand(namespace, device string) Command {
	args := []string{"netns", "exec", namespace, "ethtool", "-K", device}
	for _, feature := range offloadFeatureRequirements {
		args = append(args, feature.SetAlias, "off")
	}
	return Command{Name: "ip", Args: args}
}

// ParseOffloadFeatures normalizes `ethtool -k` feature lines. Lines such as
// the device header or subheadings which do not carry an on/off state are
// ignored.
func ParseOffloadFeatures(raw string) (map[string]OffloadFeatureState, error) {
	features := make(map[string]OffloadFeatureState)
	for _, line := range strings.Split(raw, "\n") {
		name, value, ok := strings.Cut(strings.TrimSpace(line), ":")
		if !ok || name == "" {
			continue
		}
		fields := strings.Fields(value)
		if len(fields) == 0 || (fields[0] != "on" && fields[0] != "off") {
			continue
		}
		state := OffloadFeatureState{Enabled: fields[0] == "on"}
		for _, field := range fields[1:] {
			if field == "[fixed]" {
				state.Fixed = true
			}
		}
		if previous, duplicate := features[name]; duplicate && previous != state {
			return nil, fmt.Errorf("conflicting states for feature %q", name)
		}
		features[name] = state
	}
	if len(features) == 0 {
		return nil, fmt.Errorf("no ethtool feature states found")
	}
	return features, nil
}

// ReadOffloadEvidence reads both endpoints even when the first read fails, so
// result.json preserves complete directional diagnostics.
func ReadOffloadEvidence(ctx context.Context, spec PairSpec) OffloadEvidence {
	evidence := OffloadEvidence{
		Version:          OffloadEvidenceVersion,
		RequiredFeatures: RequiredOffloadFeatures(),
		Server:           readOffloadInterface(ctx, spec.ServerNS, spec.ServerVeth),
		Client:           readOffloadInterface(ctx, spec.ClientNS, spec.ClientVeth),
	}
	evidence.SHA256 = HashOffloadEvidence(evidence)
	return evidence
}

func readOffloadInterface(ctx context.Context, namespace, device string) OffloadInterfaceEvidence {
	sample := OffloadInterfaceEvidence{Namespace: namespace, Device: device}
	cmd := exec.CommandContext(ctx, "ip", "netns", "exec", namespace, "ethtool", "-k", device)
	out, cmdErr := cmd.CombinedOutput()
	sample.Raw = string(out)
	features, parseErr := ParseOffloadFeatures(sample.Raw)
	if parseErr == nil {
		sample.Features = features
	}
	if cmdErr != nil || parseErr != nil {
		var details []string
		if cmdErr != nil {
			details = append(details, fmt.Sprintf("ethtool -k failed: %v", cmdErr))
		}
		if parseErr != nil {
			details = append(details, fmt.Sprintf("parse failed: %v", parseErr))
		}
		sample.Error = strings.Join(details, "; ")
	}
	linkCmd := exec.CommandContext(ctx, "ip", "-n", namespace, "-j", "link", "show", "dev", device)
	linkOut, linkCmdErr := linkCmd.CombinedOutput()
	sample.LinkRaw = string(linkOut)
	ifName, mtu, linkParseErr := ParseLinkMTU(sample.LinkRaw)
	if linkCmdErr == nil && linkParseErr == nil && ifName == device {
		sample.LinkMTUBytes = mtu
	} else {
		var details []string
		if sample.Error != "" {
			details = append(details, sample.Error)
		}
		if linkCmdErr != nil {
			details = append(details, fmt.Sprintf("ip link readback failed: %v", linkCmdErr))
		}
		if linkParseErr != nil {
			details = append(details, fmt.Sprintf("link readback parse failed: %v", linkParseErr))
		} else if ifName != device {
			details = append(details, fmt.Sprintf("link readback ifname=%q, want %q", ifName, device))
		}
		sample.Error = strings.Join(details, "; ")
	}
	return sample
}

// ParseLinkMTU extracts the single interface identity and MTU from `ip -j
// link show dev ...` output.
func ParseLinkMTU(raw string) (string, int, error) {
	var links []struct {
		IfName string `json:"ifname"`
		MTU    int    `json:"mtu"`
	}
	if err := json.Unmarshal([]byte(raw), &links); err != nil {
		return "", 0, err
	}
	if len(links) != 1 || links[0].IfName == "" || links[0].MTU <= 0 {
		return "", 0, fmt.Errorf("link readback must contain one named interface with a positive MTU")
	}
	return links[0].IfName, links[0].MTU, nil
}

func HashOffloadEvidence(evidence OffloadEvidence) string {
	record := struct {
		Version          int                      `json:"version"`
		RequiredFeatures []string                 `json:"required_features"`
		Server           OffloadInterfaceEvidence `json:"server"`
		Client           OffloadInterfaceEvidence `json:"client"`
	}{evidence.Version, evidence.RequiredFeatures, evidence.Server, evidence.Client}
	data, _ := json.Marshal(record)
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}

// ValidateOffloadEvidence checks both the readback semantics and its evidence
// hash. Callers decide whether offload evidence is required for a given run.
func ValidateOffloadEvidence(spec PairSpec, evidence *OffloadEvidence) []string {
	if evidence == nil {
		return []string{"missing"}
	}
	var reasons []string
	if evidence.Version != OffloadEvidenceVersion {
		reasons = append(reasons, fmt.Sprintf("version=%d, want %d", evidence.Version, OffloadEvidenceVersion))
	}
	if !slices.Equal(evidence.RequiredFeatures, RequiredOffloadFeatures()) {
		reasons = append(reasons, "required feature set does not match the benchmark contract")
	}
	if evidence.SHA256 == "" {
		reasons = append(reasons, "sha256 is missing")
	} else if actual := HashOffloadEvidence(*evidence); evidence.SHA256 != actual {
		reasons = append(reasons, fmt.Sprintf("sha256=%q does not match evidence=%q", evidence.SHA256, actual))
	}
	checks := []struct {
		role      string
		namespace string
		device    string
		sample    OffloadInterfaceEvidence
	}{
		{role: "server", namespace: spec.ServerNS, device: spec.ServerVeth, sample: evidence.Server},
		{role: "client", namespace: spec.ClientNS, device: spec.ClientVeth, sample: evidence.Client},
	}
	for _, check := range checks {
		if check.sample.Namespace != check.namespace || check.sample.Device != check.device {
			reasons = append(reasons, fmt.Sprintf("%s readback target=%s/%s, want %s/%s",
				check.role, check.sample.Namespace, check.sample.Device, check.namespace, check.device))
		}
		if check.sample.Error != "" {
			reasons = append(reasons, fmt.Sprintf("%s readback: %s", check.role, check.sample.Error))
		}
		if check.sample.Raw == "" {
			reasons = append(reasons, fmt.Sprintf("%s raw readback is empty", check.role))
		} else if parsed, err := ParseOffloadFeatures(check.sample.Raw); err != nil {
			reasons = append(reasons, fmt.Sprintf("%s raw readback cannot be parsed: %v", check.role, err))
		} else if !maps.Equal(parsed, check.sample.Features) {
			reasons = append(reasons, fmt.Sprintf("%s normalized feature map does not match raw readback", check.role))
		}
		if check.sample.LinkRaw == "" {
			reasons = append(reasons, fmt.Sprintf("%s link readback is empty", check.role))
		} else if ifName, mtu, err := ParseLinkMTU(check.sample.LinkRaw); err != nil {
			reasons = append(reasons, fmt.Sprintf("%s link readback cannot be parsed: %v", check.role, err))
		} else {
			if ifName != check.device {
				reasons = append(reasons, fmt.Sprintf("%s link readback ifname=%q, want %q", check.role, ifName, check.device))
			}
			if mtu != check.sample.LinkMTUBytes {
				reasons = append(reasons, fmt.Sprintf("%s stored link_mtu_bytes=%d does not match raw readback=%d", check.role, check.sample.LinkMTUBytes, mtu))
			}
		}
		if check.sample.LinkMTUBytes != spec.LinkMTUBytes {
			reasons = append(reasons, fmt.Sprintf("%s link_mtu_bytes=%d, want configured %d", check.role, check.sample.LinkMTUBytes, spec.LinkMTUBytes))
		}
		for _, name := range RequiredOffloadFeatures() {
			state, ok := check.sample.Features[name]
			if !ok {
				reasons = append(reasons, fmt.Sprintf("%s feature %s is unavailable", check.role, name))
			} else if state.Enabled {
				reasons = append(reasons, fmt.Sprintf("%s feature %s is on", check.role, name))
			}
		}
	}
	return reasons
}
