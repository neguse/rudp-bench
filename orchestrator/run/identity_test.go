package run

import (
	"os"
	"path/filepath"
	"testing"
)

func TestConfigIdentityTracksMeasurementInputs(t *testing.T) {
	base := RunConfig{
		Transport: "raw_udp", TotalConns: 3, OutputDir: "one",
		ServerCommand: CommandConfig{Path: "missing-server"},
		ClientCommand: CommandConfig{Path: "missing-client"},
		Scenario:      ptr(authoritativeFixture()),
	}
	a := ConfigIdentity(base)
	otherOutput := base
	otherOutput.OutputDir = "two"
	if got := ConfigIdentity(otherOutput); got != a {
		t.Fatalf("output dir changed identity: %s != %s", got, a)
	}
	changed := base
	copyScenario := *base.Scenario
	copyInput := *copyScenario.ClientInput
	copyInput.LossTolerant.RateHz++
	copyScenario.ClientInput = &copyInput
	changed.Scenario = &copyScenario
	if got := ConfigIdentity(changed); got == a {
		t.Fatal("scenario rate did not change identity")
	}
}

func TestCommandFingerprintTracksManagedBundleAndReferencedFiles(t *testing.T) {
	dir := t.TempDir()
	app := filepath.Join(dir, "Bench.Server")
	deps := filepath.Join(dir, "Bench.Server.deps.json")
	dll := filepath.Join(dir, "Bench.Server.dll")
	config := filepath.Join(dir, "scenario.json")
	for path, data := range map[string]string{
		app: "stable-apphost", deps: `{}`, dll: "implementation-v1", config: `{"rate":20}`,
	} {
		if err := os.WriteFile(path, []byte(data), 0o755); err != nil {
			t.Fatal(err)
		}
	}
	command := CommandConfig{Path: "Bench.Server", Dir: dir, Args: []string{"--config", "scenario.json"}}
	base := CommandFingerprint(command)
	if base == "" || base == "unresolved" || base == "unreadable" {
		t.Fatalf("fingerprint = %q", base)
	}
	if err := os.WriteFile(dll, []byte("implementation-v2"), 0o755); err != nil {
		t.Fatal(err)
	}
	if got := CommandFingerprint(command); got == base {
		t.Fatal("managed DLL change did not change command fingerprint")
	}
	base = CommandFingerprint(command)
	if err := os.WriteFile(config, []byte(`{"rate":60}`), 0o755); err != nil {
		t.Fatal(err)
	}
	if got := CommandFingerprint(command); got == base {
		t.Fatal("referenced config change did not change command fingerprint")
	}
}

func TestEnvironmentFingerprintIgnoresSessionMetadata(t *testing.T) {
	base := EnvironmentFingerprint()
	t.Setenv("INVOCATION_ID", "volatile-session-value")
	if got := EnvironmentFingerprint(); got != base {
		t.Fatal("volatile session metadata changed environment identity")
	}
	t.Setenv("DOTNET_GCHeapHardLimit", "123456")
	if got := EnvironmentFingerprint(); got == base {
		t.Fatal("runtime tuning did not change environment identity")
	}
}
