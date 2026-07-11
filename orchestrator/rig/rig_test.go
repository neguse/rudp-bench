package rig

import (
	"os"
	"path/filepath"
	"testing"
)

func TestLoadRejectsUnknownField(t *testing.T) {
	path := filepath.Join(t.TempDir(), "rig.json")
	data := []byte(`{
  "name":"test","os_cpus":"0","bench_cpus":"1","client_cpus":"1",
  "server_cpus":"1","all_cpus":"0-1","expected_clocksorce":"tsc"
}`)
	if err := os.WriteFile(path, data, 0o644); err != nil {
		t.Fatal(err)
	}
	if _, err := Load(path); err == nil {
		t.Fatal("unknown rig field unexpectedly accepted")
	}
}

func TestRigRejectsOverlappingRoles(t *testing.T) {
	r := Rig{Name: "bad", OSCPUs: "0", BenchCPUs: "1-2", ClientCPUs: "1-2", ServerCPUs: "2", AllCPUs: "0-2"}
	if err := r.Validate(); err == nil {
		t.Fatal("overlapping client/server CPU sets accepted")
	}
}

func TestRigRejectsGovernorAndFixedFrequencyTogether(t *testing.T) {
	r := Rig{
		Name: "bad", OSCPUs: "0", BenchCPUs: "1-2", ClientCPUs: "1", ServerCPUs: "2",
		AllCPUs: "0-2", RequirePerformanceGovernor: true, ExpectFixedFrequency: true,
	}
	if err := r.Validate(); err == nil {
		t.Fatal("mutually exclusive frequency expectations accepted")
	}
}

func TestParseCPUSetAndSubset(t *testing.T) {
	cpus, err := ParseCPUSet("3, 4,11-12")
	if err != nil || len(cpus) != 4 {
		t.Fatalf("cpus=%v err=%v", cpus, err)
	}
	if !IsSubset("3,12", "3-12") || !Intersects("1-3", "3-5") {
		t.Fatal("CPU set relation failed")
	}
}
