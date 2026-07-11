package conformance

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"strings"
	"testing"

	"github.com/neguse/rudp-bench/orchestrator/run"
)

func TestLoadSessionConfigIsStrict(t *testing.T) {
	dir := t.TempDir()
	command := writeDescribeEndpoint(t, dir, "strict", "strict", nativeSessionMapping(), "")
	config := validSessionConfig(t, dir, map[string]TransportSpec{
		"strict": {ServerCommand: command, ClientCommand: command, ClientProcs: 1},
	})
	data, err := json.Marshal(config)
	if err != nil {
		t.Fatal(err)
	}
	validPath := filepath.Join(dir, "valid.json")
	if err := os.WriteFile(validPath, data, 0o644); err != nil {
		t.Fatal(err)
	}
	loaded, err := LoadSessionConfig(validPath)
	if err != nil {
		t.Fatal(err)
	}
	if loaded.Version != SessionConfigVersion || loaded.Transports["strict"].ClientProcs != 1 {
		t.Fatalf("unexpected loaded config: %+v", loaded)
	}

	unknownPath := filepath.Join(dir, "unknown.json")
	unknown := append([]byte(`{"unknown":true,`), data[1:]...)
	if err := os.WriteFile(unknownPath, unknown, 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := LoadSessionConfig(unknownPath); err == nil || !strings.Contains(err.Error(), "unknown field") {
		t.Fatalf("unknown field error=%v", err)
	}

	trailingPath := filepath.Join(dir, "trailing.json")
	trailing := append(append([]byte(nil), data...), []byte(` {}`)...)
	if err := os.WriteFile(trailingPath, trailing, 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := LoadSessionConfig(trailingPath); err == nil || !strings.Contains(err.Error(), "exactly one JSON object") {
		t.Fatalf("trailing object error=%v", err)
	}
}

func TestCheckedInLocalSessionConfigUsesFrozenProbeAndSevenTransports(t *testing.T) {
	config, err := LoadSessionConfig("../examples/class-mapping-conformance-local.json")
	if err != nil {
		t.Fatal(err)
	}
	if config.Probe != DefaultConfig(1) {
		t.Fatalf("checked-in probe drifted: %+v", config.Probe)
	}
	if len(config.Transports) != 7 {
		t.Fatalf("transport count=%d, want 7", len(config.Transports))
	}
	if !config.Transports["raw_udp"].Diagnostic {
		t.Fatal("raw_udp must remain a diagnostic measurement floor")
	}
	for _, name := range []string{"raw_udp", "enet", "msquic", "gns", "magiconion", "websocket", "litenetlib"} {
		if _, ok := config.Transports[name]; !ok {
			t.Fatalf("checked-in config is missing transport %q", name)
		}
	}
}

func TestSessionConfigPrepareRejectsLooseTopologyAndUnsafeInputs(t *testing.T) {
	base := validSessionConfig(t, t.TempDir(), map[string]TransportSpec{
		"ok": {
			ServerCommand: run.CommandConfig{Path: "/bin/true"},
			ClientCommand: run.CommandConfig{Path: "/bin/true"},
			ClientProcs:   1,
		},
	})
	tests := []struct {
		name     string
		mutate   func(*SessionConfig)
		fragment string
	}{
		{"version", func(c *SessionConfig) { c.Version++ }, "version"},
		{"probe", func(c *SessionConfig) { c.Probe.TotalConnections = 2 }, "total_connections"},
		{"output", func(c *SessionConfig) { c.OutputDir = "" }, "output_dir"},
		{"output NUL", func(c *SessionConfig) { c.OutputDir = "bad\x00path" }, "NUL"},
		{"client procs omitted", func(c *SessionConfig) {
			s := c.Transports["ok"]
			s.ClientProcs = 0
			c.Transports["ok"] = s
		}, "want exactly 1"},
		{"client procs many", func(c *SessionConfig) {
			s := c.Transports["ok"]
			s.ClientProcs = 2
			c.Transports["ok"] = s
		}, "want exactly 1"},
		{"only diagnostics", func(c *SessionConfig) {
			s := c.Transports["ok"]
			s.Diagnostic = true
			c.Transports["ok"] = s
		}, "non-diagnostic"},
		{"unsafe name", func(c *SessionConfig) {
			c.Transports["../bad"] = c.Transports["ok"]
			delete(c.Transports, "ok")
		}, "path-safe"},
		{"missing command", func(c *SessionConfig) {
			s := c.Transports["ok"]
			s.ClientCommand.Path = ""
			c.Transports["ok"] = s
		}, "client_command.path"},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			config := cloneSessionConfig(base)
			test.mutate(&config)
			if _, err := config.Prepare(); err == nil || !strings.Contains(err.Error(), test.fragment) {
				t.Fatalf("Prepare error=%v, want %q", err, test.fragment)
			}
		})
	}
}

func TestBuildSessionPlanIsOrderIndependentAndSorted(t *testing.T) {
	dir := t.TempDir()
	alphaCommand := writeDescribeEndpoint(t, dir, "alpha", "alpha", nativeSessionMapping(), "")
	zetaCommand := writeDescribeEndpoint(t, dir, "zeta", "zeta", nativeSessionMapping(), "")
	alpha := TransportSpec{ServerCommand: alphaCommand, ClientCommand: alphaCommand, ClientProcs: 1}
	zeta := TransportSpec{ServerCommand: zetaCommand, ClientCommand: zetaCommand, ClientProcs: 1}
	first := validSessionConfig(t, dir, map[string]TransportSpec{"zeta": zeta, "alpha": alpha})
	second := validSessionConfig(t, dir, map[string]TransportSpec{"alpha": alpha, "zeta": zeta})

	firstPlan := mustBuildSessionPlan(t, first)
	secondPlan := mustBuildSessionPlan(t, second)
	if firstPlan.SessionIdentity != secondPlan.SessionIdentity || run.HashValue(firstPlan) != run.HashValue(secondPlan) {
		t.Fatal("map insertion order changed the deterministic session plan")
	}
	if got := []string{firstPlan.Preflights[0].Transport, firstPlan.Preflights[1].Transport}; !reflect.DeepEqual(got, []string{"alpha", "zeta"}) {
		t.Fatalf("preflight order=%v", got)
	}
	previousTransport := ""
	previousCase := ProbeCaseID("")
	for _, plannedCase := range firstPlan.Cases {
		if plannedCase.Transport < previousTransport ||
			(plannedCase.Transport == previousTransport && plannedCase.ID < previousCase) {
			t.Fatalf("cases are not sorted: %s/%s after %s/%s", plannedCase.Transport, plannedCase.ID, previousTransport, previousCase)
		}
		previousTransport, previousCase = plannedCase.Transport, plannedCase.ID
		for index, attempt := range plannedCase.Attempts {
			if attempt.AttemptNumber != index+1 {
				t.Fatalf("attempt order for %s/%s = %d at index %d", plannedCase.Transport, plannedCase.ID, attempt.AttemptNumber, index)
			}
		}
	}
}

func TestBuildSessionPlanPreflightsEveryTransportBeforeFailing(t *testing.T) {
	dir := t.TempDir()
	invalid := nativeSessionMapping()
	broken := invalid[run.ClassLossTolerant]
	broken.Primitive = ""
	invalid[run.ClassLossTolerant] = broken
	alphaCommand := writeDescribeEndpoint(t, dir, "bad-alpha", "alpha", invalid, "")
	zetaCommand := writeDescribeEndpoint(t, dir, "bad-zeta", "zeta", invalid, "")
	config := validSessionConfig(t, dir, map[string]TransportSpec{
		"alpha": {ServerCommand: alphaCommand, ClientCommand: alphaCommand, ClientProcs: 1},
		"zeta":  {ServerCommand: zetaCommand, ClientCommand: zetaCommand, ClientProcs: 1},
	})
	_, err := BuildSessionPlan(context.Background(), config)
	if err == nil {
		t.Fatal("invalid endpoint mappings unexpectedly passed preflight")
	}
	for _, transport := range []string{"alpha", "zeta"} {
		if !strings.Contains(err.Error(), `transport "`+transport+`" mapping preflight`) {
			t.Fatalf("preflight error does not include %s: %v", transport, err)
		}
	}
}

func TestSessionIdentityBindsTreatmentButNotArtifactLocators(t *testing.T) {
	dir := t.TempDir()
	commandPath := filepath.Join(dir, "identity-endpoint")
	command := writeDescribeEndpointAt(t, commandPath, "identity", nativeSessionMapping(), "# revision one")
	base := validSessionConfig(t, dir, map[string]TransportSpec{
		"identity": {ServerCommand: command, ClientCommand: command, ClientProcs: 1},
	})
	doctorA := filepath.Join(dir, "doctor-a.json")
	doctorB := filepath.Join(dir, "doctor-b.json")
	if err := os.WriteFile(doctorA, []byte(`{"version":1,"same":true}`), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(doctorB, []byte(`{"version":1,"same":true}`), 0o644); err != nil {
		t.Fatal(err)
	}
	base.DoctorReport = doctorA
	basePlan := mustBuildSessionPlan(t, base)

	relocated := cloneSessionConfig(base)
	relocated.OutputDir = filepath.Join(dir, "relocated-output")
	relocated.DoctorReport = doctorB
	relocatedPlan := mustBuildSessionPlan(t, relocated)
	if basePlan.SessionIdentity != relocatedPlan.SessionIdentity || basePlan.ConfigSHA256 != relocatedPlan.ConfigSHA256 {
		t.Fatal("artifact locator paths changed session identity")
	}
	for index := range basePlan.Cases {
		for attemptIndex := range basePlan.Cases[index].Attempts {
			a := basePlan.Cases[index].Attempts[attemptIndex]
			b := relocatedPlan.Cases[index].Attempts[attemptIndex]
			if a.RunIdentity != b.RunIdentity || a.SlotIdentity != b.SlotIdentity || a.AcquisitionID != b.AcquisitionID {
				t.Fatalf("artifact locator changed identities for %s/%s", a.Transport, a.CaseID)
			}
			if a.RunConfig.OutputDir == b.RunConfig.OutputDir {
				t.Fatal("test did not relocate run output")
			}
		}
	}

	changedLoss := cloneSessionConfig(base)
	changedLoss.Probe.LossPercent = 2
	if _, err := BuildSessionPlan(context.Background(), changedLoss); err == nil || !strings.Contains(err.Error(), "frozen") {
		t.Fatalf("non-canonical probe error=%v", err)
	}

	if err := os.WriteFile(doctorA, []byte(`{"version":1,"same":false}`), 0o644); err != nil {
		t.Fatal(err)
	}
	doctorPlan := mustBuildSessionPlan(t, base)
	if doctorPlan.SessionIdentity == basePlan.SessionIdentity || doctorPlan.DoctorSHA256 == basePlan.DoctorSHA256 {
		t.Fatal("doctor contents did not change session identity")
	}

	writeDescribeEndpointAt(t, commandPath, "identity", nativeSessionMapping(), "# revision two")
	binaryPlan := mustBuildSessionPlan(t, base)
	if binaryPlan.SessionIdentity == doctorPlan.SessionIdentity {
		t.Fatal("endpoint binary contents did not change session identity")
	}
	if firstSupportedAttempt(binaryPlan).RunIdentity == firstSupportedAttempt(doctorPlan).RunIdentity {
		t.Fatal("endpoint binary contents did not change run identity")
	}
}

func TestBuildSessionPlanDoesNotMutateCommandSlices(t *testing.T) {
	dir := t.TempDir()
	command := writeDescribeEndpoint(t, dir, "slices", "slices", nativeSessionMapping(), "")
	serverBacking := make([]string, 2, 16)
	serverBacking[0], serverBacking[1] = "--server-base", "server-sentinel"
	clientBacking := make([]string, 2, 16)
	clientBacking[0], clientBacking[1] = "--client-base", "client-sentinel"
	serverCommand := command
	serverCommand.Args = serverBacking[:1]
	clientCommand := command
	clientCommand.Args = clientBacking[:1]
	config := validSessionConfig(t, dir, map[string]TransportSpec{
		"slices": {ServerCommand: serverCommand, ClientCommand: clientCommand, ClientProcs: 1},
	})
	_ = mustBuildSessionPlan(t, config)
	if got := serverBacking[:2]; !reflect.DeepEqual(got, []string{"--server-base", "server-sentinel"}) {
		t.Fatalf("server args backing array mutated: %v", got)
	}
	if got := clientBacking[:2]; !reflect.DeepEqual(got, []string{"--client-base", "client-sentinel"}) {
		t.Fatalf("client args backing array mutated: %v", got)
	}
	transport := config.Transports["slices"]
	if !reflect.DeepEqual(transport.ServerCommand.Args, []string{"--server-base"}) ||
		!reflect.DeepEqual(transport.ClientCommand.Args, []string{"--client-base"}) {
		t.Fatalf("caller command args changed: server=%v client=%v", transport.ServerCommand.Args, transport.ClientCommand.Args)
	}
}

func TestSessionPlanHasExactFixedCaseShapesAndIdentities(t *testing.T) {
	dir := t.TempDir()
	command := writeDescribeEndpoint(t, dir, "shape", "shape", nativeSessionMapping(), "")
	config := validSessionConfig(t, dir, map[string]TransportSpec{
		"shape": {ServerCommand: command, ClientCommand: command, ClientProcs: 1, SchedIsMeasurand: true},
	})
	plan := mustBuildSessionPlan(t, config)
	if len(plan.Cases) != 6 {
		t.Fatalf("case count=%d, want 6", len(plan.Cases))
	}
	wantDefinitions := RequiredCases()
	sort.Slice(wantDefinitions, func(i, j int) bool { return wantDefinitions[i].ID < wantDefinitions[j].ID })
	prefixes := map[string]bool{}
	identities := map[string]bool{}
	for index, plannedCase := range plan.Cases {
		definition := wantDefinitions[index]
		if plannedCase.CaseDefinition != definition {
			t.Fatalf("case[%d]=%+v, want %+v", index, plannedCase.CaseDefinition, definition)
		}
		if plannedCase.Unsupported || len(plannedCase.Attempts) != config.Probe.MaxAttemptsPerCase {
			t.Fatalf("case %s unsupported=%v attempts=%d", plannedCase.ID, plannedCase.Unsupported, len(plannedCase.Attempts))
		}
		if plannedCase.RequiredAcquisitions != config.Probe.ValidAcquisitionsPerCase {
			t.Fatalf("case %s required acquisitions=%d", plannedCase.ID, plannedCase.RequiredAcquisitions)
		}
		for _, attempt := range plannedCase.Attempts {
			assertFixedAttemptShape(t, config.Probe, definition, attempt)
			if attempt.RunIdentity != run.ConfigIdentity(attempt.RunConfig) {
				t.Fatalf("run identity drift for %s attempt %d", definition.ID, attempt.AttemptNumber)
			}
			if want := AcquisitionIdentity(plannedCase.CaseIdentity, attempt.AttemptNumber, attempt.RunIdentity); attempt.AcquisitionID != want {
				t.Fatalf("acquisition identity=%q, want shared contract %q", attempt.AcquisitionID, want)
			}
			for name, digest := range map[string]string{
				"run": attempt.RunIdentity, "slot": attempt.SlotIdentity, "acquisition": attempt.AcquisitionID,
			} {
				if !isSHA256Digest(digest) {
					t.Fatalf("%s identity=%q", name, digest)
				}
				key := name + ":" + digest
				if identities[key] {
					t.Fatalf("duplicate %s identity %q", name, digest)
				}
				identities[key] = true
			}
			prefix := attempt.RunConfig.Netem.Prefix
			if prefixes[prefix] {
				t.Fatalf("duplicate netns prefix %q", prefix)
			}
			prefixes[prefix] = true
		}
	}
}

func TestSessionPlanSkipsUnsupportedRawMustDeliver(t *testing.T) {
	dir := t.TempDir()
	command := writeDescribeEndpoint(t, dir, "raw", "raw", rawSessionMapping(), "")
	config := validSessionConfig(t, dir, map[string]TransportSpec{
		"raw": {ServerCommand: command, ClientCommand: command, ClientProcs: 1},
	})
	plan := mustBuildSessionPlan(t, config)
	unsupported := 0
	for _, plannedCase := range plan.Cases {
		if plannedCase.Class == run.ClassMustDeliver {
			unsupported++
			if !plannedCase.Unsupported || len(plannedCase.Attempts) != 0 || len(plannedCase.SkipReasons) == 0 {
				t.Fatalf("must-deliver case was not skipped: %+v", plannedCase)
			}
			if plannedCase.DeclaredMapping == nil || plannedCase.DeclaredMapping.Realization != run.ClassMappingRealizationUnsupported {
				t.Fatalf("missing unsupported declaration: %+v", plannedCase.DeclaredMapping)
			}
		} else if plannedCase.Unsupported || len(plannedCase.Attempts) != config.Probe.MaxAttemptsPerCase {
			t.Fatalf("loss-tolerant case unexpectedly skipped: %+v", plannedCase)
		}
	}
	if unsupported != 3 {
		t.Fatalf("unsupported cases=%d, want 3", unsupported)
	}
}

func assertFixedAttemptShape(t *testing.T, probe Config, definition CaseDefinition, attempt AttemptPlan) {
	t.Helper()
	config := attempt.RunConfig
	if config.Scenario == nil || config.Scenario.Kind != run.ScenarioEnvironmentBaseline || config.Scenario.ClientInput == nil {
		t.Fatalf("case %s scenario=%+v", definition.ID, config.Scenario)
	}
	if config.ClientProcs != 1 || config.TotalConns != 1 || !config.SchedIsMeasurand {
		t.Fatalf("case %s topology client_procs=%d total_conns=%d sched=%v", definition.ID, config.ClientProcs, config.TotalConns, config.SchedIsMeasurand)
	}
	traffic := config.Scenario.ClientInput
	if traffic.TrafficID != run.TrafficIDClientInput {
		t.Fatalf("case %s traffic id=%d", definition.ID, traffic.TrafficID)
	}
	wantClass := run.TrafficClassSpec{RateHz: float64(probe.RateHz), PayloadBytes: int(probe.PayloadBytes)}
	if definition.Class == run.ClassLossTolerant {
		if traffic.LossTolerant != wantClass || traffic.MustDeliver != (run.TrafficClassSpec{}) {
			t.Fatalf("case %s classes=%+v", definition.ID, traffic)
		}
	} else if traffic.MustDeliver != wantClass || traffic.LossTolerant != (run.TrafficClassSpec{}) {
		t.Fatalf("case %s classes=%+v", definition.ID, traffic)
	}
	if config.Netem == nil || !config.Netem.DisableOffloads || config.Netem.Prefix == "" {
		t.Fatalf("case %s netem=%+v", definition.ID, config.Netem)
	}
	if config.Netem.LinkMTUBytes != int(probe.LinkMTUBytes) {
		t.Fatalf("case %s link MTU=%d, want %d", definition.ID, config.Netem.LinkMTUBytes, probe.LinkMTUBytes)
	}
	if config.AttemptedThreshold != 1 {
		t.Fatalf("case %s attempted threshold=%g, want 1", definition.ID, config.AttemptedThreshold)
	}
	clientLoss := config.Netem.ClientEgress.LossPercent
	serverLoss := config.Netem.ServerEgress.LossPercent
	switch definition.Egress {
	case EgressNone:
		if clientLoss != 0 || serverLoss != 0 {
			t.Fatalf("clean case %s has loss client=%g server=%g", definition.ID, clientLoss, serverLoss)
		}
	case EgressClient:
		if clientLoss != probe.LossPercent || serverLoss != 0 {
			t.Fatalf("client-loss case %s has client=%g server=%g", definition.ID, clientLoss, serverLoss)
		}
	case EgressServer:
		if serverLoss != probe.LossPercent || clientLoss != 0 {
			t.Fatalf("server-loss case %s has client=%g server=%g", definition.ID, clientLoss, serverLoss)
		}
	}
	for name, netem := range map[string]struct {
		delay, jitter int
		burst         float64
		seed          uint64
		rate          string
	}{
		"client": {config.Netem.ClientEgress.DelayMS, config.Netem.ClientEgress.JitterMS, config.Netem.ClientEgress.LossBurstLen, config.Netem.ClientEgress.LossSeed, config.Netem.ClientEgress.Rate},
		"server": {config.Netem.ServerEgress.DelayMS, config.Netem.ServerEgress.JitterMS, config.Netem.ServerEgress.LossBurstLen, config.Netem.ServerEgress.LossSeed, config.Netem.ServerEgress.Rate},
	} {
		if netem.delay != 0 || netem.jitter != 0 || netem.burst != 0 || netem.seed != 0 || netem.rate != "" {
			t.Fatalf("case %s %s netem is not random-loss-only: %+v", definition.ID, name, netem)
		}
	}
	if uint64(config.Warmup.Duration) != probe.WarmupNS || uint64(config.Duration.Duration) != probe.DurationNS || uint64(config.Drain.Duration) != probe.DrainNS {
		t.Fatalf("case %s schedule warmup=%s duration=%s drain=%s", definition.ID, config.Warmup.Duration, config.Duration.Duration, config.Drain.Duration)
	}
}

func validSessionConfig(t *testing.T, output string, transports map[string]TransportSpec) SessionConfig {
	t.Helper()
	return SessionConfig{
		Version:    SessionConfigVersion,
		Probe:      DefaultConfig(1),
		Transports: transports,
		OutputDir:  filepath.Join(output, "session-output"),
	}
}

func nativeSessionMapping() map[string]run.ClassMappingSpec {
	return map[string]run.ClassMappingSpec{
		run.ClassLossTolerant: {
			Primitive: "native-datagram", Delivery: run.ClassMappingDeliveryBestEffort,
			Ordering: run.ClassMappingOrderingUnordered, Realization: run.ClassMappingRealizationNative,
		},
		run.ClassMustDeliver: {
			Primitive: "native-reliable", Delivery: run.ClassMappingDeliveryReliable,
			Ordering: run.ClassMappingOrderingOrdered, Realization: run.ClassMappingRealizationNative,
		},
	}
}

func rawSessionMapping() map[string]run.ClassMappingSpec {
	mapping := nativeSessionMapping()
	mapping[run.ClassLossTolerant] = run.ClassMappingSpec{
		Primitive: "udp", Delivery: run.ClassMappingDeliveryBestEffort,
		Ordering: run.ClassMappingOrderingUnordered, Realization: run.ClassMappingRealizationNative,
	}
	mapping[run.ClassMustDeliver] = run.ClassMappingSpec{
		Primitive: "udp", Delivery: run.ClassMappingDeliveryBestEffort,
		Ordering: run.ClassMappingOrderingUnordered, Realization: run.ClassMappingRealizationUnsupported,
	}
	return mapping
}

func writeDescribeEndpoint(t *testing.T, dir, fileName, transport string, mapping map[string]run.ClassMappingSpec, revision string) run.CommandConfig {
	t.Helper()
	return writeDescribeEndpointAt(t, filepath.Join(dir, fileName), transport, mapping, revision)
}

func writeDescribeEndpointAt(t *testing.T, path, transport string, mapping map[string]run.ClassMappingSpec, revision string) run.CommandConfig {
	t.Helper()
	description, err := json.Marshal(struct {
		Transport    string                          `json:"transport"`
		ClassMapping map[string]run.ClassMappingSpec `json:"class_mapping"`
	}{transport, mapping})
	if err != nil {
		t.Fatal(err)
	}
	script := "#!/bin/sh\n" + revision + "\n" +
		"if [ \"$1\" = \"--describe\" ]; then\n" +
		"  printf '%s\\n' '" + string(description) + "'\n" +
		"  exit 0\n" +
		"fi\nexit 1\n"
	if err := os.WriteFile(path, []byte(script), 0o755); err != nil {
		t.Fatal(err)
	}
	return run.CommandConfig{Path: path}
}

func mustBuildSessionPlan(t *testing.T, config SessionConfig) SessionPlan {
	t.Helper()
	plan, err := BuildSessionPlan(context.Background(), config)
	if err != nil {
		t.Fatal(err)
	}
	return plan
}

func firstSupportedAttempt(plan SessionPlan) AttemptPlan {
	for _, plannedCase := range plan.Cases {
		if len(plannedCase.Attempts) != 0 {
			return plannedCase.Attempts[0]
		}
	}
	return AttemptPlan{}
}
