package doctor

import (
	"archive/tar"
	"context"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/rig"
)

func TestParseCPUSet(t *testing.T) {
	got, err := parseCPUSet("3,4,11-14,3")
	if err != nil {
		t.Fatal(err)
	}
	want := []int{3, 4, 11, 12, 13, 14}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("cpus = %v, want %v", got, want)
	}
	if !equalCPUSet("0-2,8-10", "10,9,8,2,1,0") {
		t.Fatal("equivalent CPU sets differ")
	}
}

func TestGovernorCheckTargetsBenchCPUs(t *testing.T) {
	r := rig.Rig{BenchCPUs: "3-4", RequirePerformanceGovernor: true}
	check := governorCheck(r, map[string]string{"0": "powersave", "3": "performance", "4": "performance"})
	if check.Status != StatusPass {
		t.Fatalf("check = %+v", check)
	}
	check = governorCheck(r, map[string]string{"3": "powersave", "4": "performance"})
	if check.Status != StatusFail {
		t.Fatalf("check = %+v", check)
	}
}

func TestIRQAffinityConflictsUsesEffectiveMask(t *testing.T) {
	root := t.TempDir()
	writeIRQ := func(number, requested string, effective *string) {
		t.Helper()
		dir := filepath.Join(root, number)
		if err := os.Mkdir(dir, 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(dir, "smp_affinity_list"), []byte(requested+"\n"), 0o644); err != nil {
			t.Fatal(err)
		}
		if effective != nil {
			if err := os.WriteFile(filepath.Join(dir, "effective_affinity_list"), []byte(*effective+"\n"), 0o644); err != nil {
				t.Fatal(err)
			}
		}
	}
	zero := "0"
	empty := ""
	eleven := "11"
	writeIRQ("0", "0-15", &zero)
	writeIRQ("2", "0-15", &empty)
	writeIRQ("54", "3,11", &eleven)
	writeIRQ("55", "4", nil) // Older kernels: fall back to the requested mask.

	got := irqAffinityConflicts(root, "3-7,11-15")
	want := []string{"54:11", "55:4"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("conflicts = %v, want %v", got, want)
	}
}

func TestCaptureSourceSnapshotPreservesDirtyWorktree(t *testing.T) {
	if _, err := exec.LookPath("git"); err != nil {
		t.Skip("git is not available")
	}
	repo := t.TempDir()
	runGit(t, repo, "init", "--quiet")
	tracked := filepath.Join(repo, "tracked.txt")
	if err := os.WriteFile(tracked, []byte("base\n"), 0o644); err != nil {
		t.Fatal(err)
	}
	runGit(t, repo, "add", "tracked.txt")
	runGit(t, repo, "-c", "user.name=benchmark", "-c", "user.email=benchmark@example.invalid", "commit", "--quiet", "-m", "base")
	wantTracked := []byte("changed with trailing spaces  \n")
	if err := os.WriteFile(tracked, wantTracked, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(repo, "untracked.txt"), []byte("new source\n"), 0o640); err != nil {
		t.Fatal(err)
	}

	reportPath := filepath.Join(t.TempDir(), "doctor.json")
	report := Report{}
	if err := CaptureSourceSnapshot(context.Background(), repo, reportPath, &report); err != nil {
		t.Fatal(err)
	}
	if !report.Git.Dirty || report.Git.PatchSHA256 == "" || report.Git.SourceSnapshot == nil {
		t.Fatalf("incomplete source snapshot: %+v", report.Git)
	}
	snapshot := report.Git.SourceSnapshot
	if len(snapshot.UntrackedFiles) != 1 || snapshot.UntrackedFiles[0].Path != "untracked.txt" {
		t.Fatalf("untracked manifest = %+v", snapshot.UntrackedFiles)
	}
	patch, err := os.ReadFile(filepath.Join(filepath.Dir(reportPath), snapshot.TrackedPatch.Path))
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(patch), "+changed with trailing spaces  \n") {
		t.Fatalf("tracked patch does not contain the modification:\n%s", patch)
	}
	runGit(t, repo, "checkout", "--", "tracked.txt")
	runGit(t, repo, "apply", filepath.Join(filepath.Dir(reportPath), snapshot.TrackedPatch.Path))
	gotTracked, err := os.ReadFile(tracked)
	if err != nil {
		t.Fatal(err)
	}
	if string(gotTracked) != string(wantTracked) {
		t.Fatalf("raw patch did not reconstruct tracked bytes: got %q want %q", gotTracked, wantTracked)
	}

	archive, err := os.Open(filepath.Join(filepath.Dir(reportPath), snapshot.UntrackedArchive.Path))
	if err != nil {
		t.Fatal(err)
	}
	defer archive.Close()
	tr := tar.NewReader(archive)
	header, err := tr.Next()
	if err != nil {
		t.Fatal(err)
	}
	content, err := io.ReadAll(tr)
	if err != nil {
		t.Fatal(err)
	}
	if header.Name != "untracked.txt" || string(content) != "new source\n" {
		t.Fatalf("untracked archive entry = %q %q", header.Name, content)
	}
	if _, err := tr.Next(); err != io.EOF {
		t.Fatalf("unexpected second archive entry: %v", err)
	}
}

func TestCollectRejectsNonGitSourceDirectory(t *testing.T) {
	report := Collect(context.Background(), rig.Rig{}, t.TempDir())
	if report.OK {
		t.Fatal("non-Git source directory unexpectedly passed Doctor")
	}
	for _, check := range report.Checks {
		if check.Name == "source_state" {
			if check.Status != StatusFail {
				t.Fatalf("source_state check = %+v", check)
			}
			return
		}
	}
	t.Fatal("source_state check is missing")
}

func TestValidateReferenceRequiresSourceCommit(t *testing.T) {
	now := time.Now().UTC()
	checks := make([]Check, len(referenceCheckNames))
	for index, name := range referenceCheckNames {
		checks[index] = Check{Name: name, Status: StatusPass}
	}
	report := Report{
		Version: 1, OK: true, GeneratedAt: now,
		Rig: rig.Rig{
			Name:                       "test",
			OSCPUs:                     "0",
			BenchCPUs:                  "1-2",
			ClientCPUs:                 "1",
			ServerCPUs:                 "2",
			AllCPUs:                    "0-2",
			ExpectedClocksource:        "tsc",
			RequirePerformanceGovernor: true,
			RequireIsolation:           true,
			MinNoFile:                  1,
		},
		Checks: checks,
	}
	if err := ValidateReferenceReport(report, now); err == nil || !strings.Contains(err.Error(), "source commit") {
		t.Fatalf("expected missing source commit error, got %v", err)
	}
}

func TestBenchmarkNetworkNamespaceDetectionIncludesConformancePrefixes(t *testing.T) {
	tests := map[string]bool{
		"rudpbench-srv":         true,
		"rudpbench-old-run":     true,
		"cm012345678-srv":       true,
		"cmabcdef012-cli":       true,
		"cmABCDEF012-srv":       false,
		"cm01234567-srv":        false,
		"cm012345678-worker":    false,
		"other-cm012345678-srv": false,
	}
	for name, want := range tests {
		if got := isBenchmarkNetworkNamespace(name); got != want {
			t.Errorf("isBenchmarkNetworkNamespace(%q)=%v, want %v", name, got, want)
		}
	}
}

func runGit(t *testing.T, dir string, args ...string) {
	t.Helper()
	cmd := exec.Command("git", args...)
	cmd.Dir = dir
	if output, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("git %s: %v\n%s", strings.Join(args, " "), err, output)
	}
}
