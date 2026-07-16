// Package doctor captures and gates the measurement host before a benchmark
// campaign. It deliberately separates rig validity from transport quality.
package doctor

import (
	"archive/tar"
	"bytes"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/neguse/rudp-bench/orchestrator/rig"
	"github.com/neguse/rudp-bench/orchestrator/run"
)

const (
	StatusPass = "PASS"
	StatusFail = "FAIL"
	StatusWarn = "WARN"
)

type Check struct {
	Name     string `json:"name"`
	Status   string `json:"status"`
	Observed string `json:"observed,omitempty"`
	Expected string `json:"expected,omitempty"`
	Detail   string `json:"detail,omitempty"`
}

type GitState struct {
	Commit         string          `json:"commit,omitempty"`
	Dirty          bool            `json:"dirty"`
	PatchSHA256    string          `json:"patch_sha256,omitempty"`
	SourceSnapshot *SourceSnapshot `json:"source_snapshot,omitempty"`
	Bundle         *BundleState    `json:"bundle,omitempty"`
}

// BundleState は git worktree の代わりに bundle manifest を source 証跡と
// した場合の来歴。commit は manifest の申告値であり、バイナリ実体は
// manifest の sha256 と照合済みであることを意味する。
type BundleState struct {
	RID            string `json:"rid,omitempty"`
	ManifestSHA256 string `json:"manifest_sha256"`
	Binaries       int    `json:"binaries"`
}

type SourceArtifact struct {
	Path   string `json:"path"`
	SHA256 string `json:"sha256"`
	Size   int64  `json:"size"`
}

type SourceFile struct {
	Path       string `json:"path"`
	Mode       uint32 `json:"mode"`
	Size       int64  `json:"size"`
	SHA256     string `json:"sha256"`
	LinkTarget string `json:"link_target,omitempty"`
}

type SourceSnapshot struct {
	Status           SourceArtifact `json:"status"`
	TrackedPatch     SourceArtifact `json:"tracked_patch"`
	UntrackedArchive SourceArtifact `json:"untracked_archive"`
	UntrackedFiles   []SourceFile   `json:"untracked_files"`
}

type Report struct {
	Version            int               `json:"version"`
	GeneratedAt        time.Time         `json:"generated_at"`
	OK                 bool              `json:"ok"`
	Rig                rig.Rig           `json:"rig"`
	Hostname           string            `json:"hostname,omitempty"`
	KernelRelease      string            `json:"kernel_release,omitempty"`
	Architecture       string            `json:"architecture"`
	OnlineCPUs         string            `json:"online_cpus,omitempty"`
	Clocksource        string            `json:"clocksource,omitempty"`
	AvailableClocks    []string          `json:"available_clocksources,omitempty"`
	Governors          map[string]string `json:"governors,omitempty"`
	PID1AllowedCPUs    string            `json:"pid1_allowed_cpus,omitempty"`
	SliceAllowedCPUs   map[string]string `json:"slice_allowed_cpus,omitempty"`
	BenchIRQs          []string          `json:"bench_cpu_irqs,omitempty"`
	NetworkNamespaces  []string          `json:"network_namespaces,omitempty"`
	NoFileSoft         uint64            `json:"nofile_soft"`
	NoFileHard         uint64            `json:"nofile_hard"`
	Qdisc              string            `json:"qdisc,omitempty"`
	Tools              map[string]string `json:"tools,omitempty"`
	Git                GitState          `json:"git"`
	OrchestratorSHA256 string            `json:"orchestrator_sha256,omitempty"`
	Checks             []Check           `json:"checks"`
}

func Collect(ctx context.Context, r rig.Rig, repoDir string) Report {
	report := Report{
		Version: 1, GeneratedAt: time.Now().UTC(), OK: true, Rig: r,
		Architecture: runtime.GOARCH, Governors: map[string]string{}, Tools: map[string]string{},
		SliceAllowedCPUs:   map[string]string{},
		OrchestratorSHA256: run.OrchestratorFingerprint(),
	}
	report.Hostname, _ = os.Hostname()
	report.KernelRelease = readTrimmed("/proc/sys/kernel/osrelease")
	report.OnlineCPUs = readTrimmed("/sys/devices/system/cpu/online")
	report.Clocksource = readTrimmed("/sys/devices/system/clocksource/clocksource0/current_clocksource")
	report.AvailableClocks = strings.Fields(readTrimmed("/sys/devices/system/clocksource/clocksource0/available_clocksource"))
	report.Governors = readGovernors()
	report.PID1AllowedCPUs = readStatusField("/proc/1/status", "Cpus_allowed_list")
	var limit syscall.Rlimit
	if err := syscall.Getrlimit(syscall.RLIMIT_NOFILE, &limit); err == nil {
		report.NoFileSoft, report.NoFileHard = limit.Cur, limit.Max
	}

	if r.ExpectedClocksource == "" {
		report.add(Check{Name: "clocksource", Status: StatusWarn, Observed: report.Clocksource, Detail: "rig does not declare expected_clocksource"})
	} else {
		status := StatusPass
		if report.Clocksource != r.ExpectedClocksource {
			status = StatusFail
		}
		report.add(Check{Name: "clocksource", Status: status, Observed: report.Clocksource, Expected: r.ExpectedClocksource})
	}
	onlineStatus := StatusPass
	if !rig.IsSubset(r.AllCPUs, report.OnlineCPUs) {
		onlineStatus = StatusFail
	}
	report.add(Check{Name: "rig_cpu_layout", Status: onlineStatus, Observed: report.OnlineCPUs, Expected: "contains " + r.AllCPUs})

	report.add(governorCheck(r, report.Governors))
	if r.RequireIsolation {
		status := StatusPass
		if !equalCPUSet(report.PID1AllowedCPUs, r.OSCPUs) {
			status = StatusFail
		}
		report.add(Check{Name: "pid1_cpu_isolation", Status: status, Observed: report.PID1AllowedCPUs, Expected: r.OSCPUs})
		for _, unit := range []string{"system.slice", "user.slice", "init.scope"} {
			allowed, err := commandOutput(ctx, repoDir, "systemctl", "show", "--property", "AllowedCPUs", "--value", unit)
			report.SliceAllowedCPUs[unit] = allowed
			status := StatusPass
			detail := ""
			if err != nil || allowed == "" || !equalCPUSet(allowed, r.OSCPUs) {
				status = StatusFail
				if err != nil {
					detail = err.Error()
				}
			}
			report.add(Check{Name: "slice_cpu_isolation_" + strings.TrimSuffix(unit, ".slice"), Status: status, Observed: allowed, Expected: r.OSCPUs, Detail: detail})
		}
		report.BenchIRQs = benchCPUIRQs(r.BenchCPUs)
		irqStatus := StatusPass
		if len(report.BenchIRQs) > 0 {
			irqStatus = StatusFail
		}
		report.add(Check{Name: "irq_cpu_isolation", Status: irqStatus, Observed: strings.Join(report.BenchIRQs, ","), Expected: "no IRQ affinity intersects bench_cpus"})
	} else {
		report.add(Check{Name: "pid1_cpu_isolation", Status: StatusWarn, Observed: report.PID1AllowedCPUs, Detail: "rig does not require isolation"})
	}
	if r.MinNoFile > 0 {
		status := StatusPass
		if report.NoFileSoft < r.MinNoFile {
			status = StatusFail
		}
		report.add(Check{Name: "nofile", Status: status, Observed: strconv.FormatUint(report.NoFileSoft, 10), Expected: ">=" + strconv.FormatUint(r.MinNoFile, 10)})
	} else {
		report.add(Check{Name: "nofile", Status: StatusWarn, Observed: strconv.FormatUint(report.NoFileSoft, 10), Detail: "rig does not declare min_nofile"})
	}

	qdisc, err := commandOutput(ctx, repoDir, "tc", "qdisc", "show")
	report.Qdisc = qdisc
	qdiscCheck := Check{Name: "residual_netem", Status: StatusPass, Observed: "none"}
	if err != nil {
		qdiscCheck.Status = StatusFail
		qdiscCheck.Observed = err.Error()
		qdiscCheck.Detail = "could not verify qdisc state"
	} else if strings.Contains(qdisc, "netem") || strings.Contains(qdisc, "rudpbench") {
		qdiscCheck.Status = StatusFail
		qdiscCheck.Observed = "netem qdisc present"
	}
	report.add(qdiscCheck)

	netns, netnsErr := commandOutput(ctx, repoDir, "ip", "netns", "list")
	netnsCheck := Check{Name: "residual_benchmark_netns", Status: StatusPass, Observed: "none"}
	if netnsErr != nil {
		netnsCheck.Status = StatusFail
		netnsCheck.Observed = netnsErr.Error()
	} else {
		for _, line := range strings.Split(netns, "\n") {
			fields := strings.Fields(line)
			if len(fields) == 0 {
				continue
			}
			report.NetworkNamespaces = append(report.NetworkNamespaces, fields[0])
			if isBenchmarkNetworkNamespace(fields[0]) {
				netnsCheck.Status = StatusFail
				netnsCheck.Observed = strings.Join(report.NetworkNamespaces, ",")
			}
		}
	}
	report.add(netnsCheck)

	for _, tool := range []string{"ip", "tc", "ethtool", "ping", "iperf3", "jq", "sha256sum"} {
		path, err := exec.LookPath(tool)
		status := StatusPass
		if err != nil {
			status = StatusFail
			path = "not found"
		}
		report.Tools[tool] = path
		report.add(Check{Name: "tool_" + tool, Status: status, Observed: path, Expected: "available"})
	}
	gitState, err := collectGit(ctx, repoDir)
	sourceCheck := Check{Name: "source_state", Status: StatusPass, Observed: gitState.Commit, Expected: "readable Git worktree or verified bundle manifest"}
	if err != nil {
		bundleState, bundleErr := collectBundle(repoDir)
		if bundleErr == nil {
			gitState = bundleState
			sourceCheck.Observed = gitState.Commit
			sourceCheck.Detail = "bundle manifest verified"
		} else {
			sourceCheck.Status = StatusFail
			sourceCheck.Detail = fmt.Sprintf("git: %v; bundle: %v", err, bundleErr)
		}
	}
	report.Git = gitState
	report.add(sourceCheck)
	return report
}

type bundleManifest struct {
	Commit   string `json:"commit"`
	RID      string `json:"rid"`
	Dirty    bool   `json:"dirty"`
	Binaries []struct {
		Path   string `json:"path"`
		SHA256 string `json:"sha256"`
	} `json:"binaries"`
}

// collectBundle は bundle 展開ツリー(git worktree なし)の source 証跡を
// bundle-manifest.json から検証して集める。manifest の申告 commit を信用する
// 前提条件として、記載された全バイナリの sha256 が実体と一致することを課す。
func collectBundle(repoDir string) (GitState, error) {
	raw, err := os.ReadFile(filepath.Join(repoDir, "bundle-manifest.json"))
	if err != nil {
		return GitState{}, fmt.Errorf("bundle manifest is not readable: %w", err)
	}
	var manifest bundleManifest
	if err := json.Unmarshal(raw, &manifest); err != nil {
		return GitState{}, fmt.Errorf("bundle manifest is not valid JSON: %w", err)
	}
	if !isGitCommit(manifest.Commit) {
		return GitState{}, fmt.Errorf("bundle manifest commit %q is not a git commit hash", manifest.Commit)
	}
	if manifest.Dirty {
		return GitState{}, fmt.Errorf("bundle was built from a dirty tree")
	}
	if len(manifest.Binaries) == 0 {
		return GitState{}, fmt.Errorf("bundle manifest lists no binaries")
	}
	for _, binary := range manifest.Binaries {
		if binary.Path == "" || filepath.IsAbs(binary.Path) || binary.Path != filepath.Clean(binary.Path) || strings.HasPrefix(binary.Path, "..") {
			return GitState{}, fmt.Errorf("bundle manifest path %q is not a clean repo-relative path", binary.Path)
		}
		data, err := os.ReadFile(filepath.Join(repoDir, binary.Path))
		if err != nil {
			return GitState{}, fmt.Errorf("bundle binary %s is not readable: %w", binary.Path, err)
		}
		digest := sha256.Sum256(data)
		if !strings.EqualFold(hex.EncodeToString(digest[:]), binary.SHA256) {
			return GitState{}, fmt.Errorf("bundle binary %s does not match its manifest sha256", binary.Path)
		}
	}
	manifestDigest := sha256.Sum256(raw)
	return GitState{
		Commit: manifest.Commit,
		Bundle: &BundleState{
			RID:            manifest.RID,
			ManifestSHA256: hex.EncodeToString(manifestDigest[:]),
			Binaries:       len(manifest.Binaries),
		},
	}, nil
}

func (r *Report) add(check Check) {
	r.Checks = append(r.Checks, check)
	if check.Status == StatusFail {
		r.OK = false
	}
}

func Write(path string, report Report) error {
	data, err := json.MarshalIndent(report, "", "  ")
	if err != nil {
		return err
	}
	if path == "" {
		_, err = os.Stdout.Write(append(data, '\n'))
		return err
	}
	if dir := filepath.Dir(path); dir != "." {
		if err := os.MkdirAll(dir, 0o755); err != nil {
			return err
		}
	}
	return os.WriteFile(path, append(data, '\n'), 0o644)
}

// CaptureSourceSnapshot persists enough source material to reconstruct a dirty
// worktree. Artifact paths are relative to the doctor report directory.
func CaptureSourceSnapshot(ctx context.Context, repoDir, reportPath string, report *Report) error {
	if report == nil {
		return fmt.Errorf("doctor report is nil")
	}
	if reportPath == "" {
		return fmt.Errorf("source snapshot requires a doctor output path")
	}
	state, err := readSourceState(ctx, repoDir)
	if err != nil {
		return err
	}

	dir := filepath.Dir(reportPath)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		return err
	}
	stem := strings.TrimSuffix(filepath.Base(reportPath), filepath.Ext(reportPath))
	statusName := stem + ".source-status.txt"
	patchName := stem + ".source.patch"
	archiveName := stem + ".untracked.tar"
	statusPath := filepath.Join(dir, statusName)
	patchPath := filepath.Join(dir, patchName)
	archivePath := filepath.Join(dir, archiveName)
	if err := os.WriteFile(statusPath, state.status, 0o644); err != nil {
		return fmt.Errorf("write source status: %w", err)
	}
	if err := os.WriteFile(patchPath, state.patch, 0o644); err != nil {
		return fmt.Errorf("write tracked source patch: %w", err)
	}

	archiveFile, err := os.Create(archivePath)
	if err != nil {
		return fmt.Errorf("create untracked source archive: %w", err)
	}
	archiveHash := sha256.New()
	tw := tar.NewWriter(io.MultiWriter(archiveFile, archiveHash))
	var files []SourceFile
	for _, entry := range state.untracked {
		if err := tw.WriteHeader(entry.header); err != nil {
			_ = tw.Close()
			_ = archiveFile.Close()
			return fmt.Errorf("archive untracked source %s: %w", entry.manifest.Path, err)
		}
		if entry.header.Typeflag == tar.TypeReg {
			if _, err := tw.Write(entry.content); err != nil {
				_ = tw.Close()
				_ = archiveFile.Close()
				return fmt.Errorf("archive untracked source %s: %w", entry.manifest.Path, err)
			}
		}
		files = append(files, entry.manifest)
	}
	if err := tw.Close(); err != nil {
		_ = archiveFile.Close()
		return fmt.Errorf("finalize untracked source archive: %w", err)
	}
	if err := archiveFile.Close(); err != nil {
		return fmt.Errorf("close untracked source archive: %w", err)
	}

	report.Git = GitState{
		Commit:      state.commit,
		Dirty:       len(state.status) != 0,
		PatchSHA256: state.sha256,
		SourceSnapshot: &SourceSnapshot{
			Status:           sourceArtifact(statusName, state.status),
			TrackedPatch:     sourceArtifact(patchName, state.patch),
			UntrackedArchive: sourceArtifactFromHash(archiveName, archiveHash.Sum(nil), archivePath),
			UntrackedFiles:   files,
		},
	}
	return nil
}

type sourceEntry struct {
	manifest SourceFile
	content  []byte
	header   *tar.Header
}

type sourceState struct {
	commit    string
	status    []byte
	patch     []byte
	untracked []sourceEntry
	sha256    string
}

func readSourceState(ctx context.Context, repoDir string) (sourceState, error) {
	var state sourceState
	commit, err := gitOutput(ctx, repoDir, "rev-parse", "HEAD")
	if err != nil {
		return state, fmt.Errorf("read source commit: %w", err)
	}
	state.commit = strings.TrimSpace(string(commit))
	if state.commit == "" {
		return state, fmt.Errorf("source commit is empty")
	}
	state.status, err = gitOutput(ctx, repoDir, "status", "--porcelain=v1")
	if err != nil {
		return state, fmt.Errorf("read source status: %w", err)
	}
	state.patch, err = gitOutput(ctx, repoDir, "diff", "--binary", "HEAD")
	if err != nil {
		return state, fmt.Errorf("read tracked source patch: %w", err)
	}
	submoduleStatus, err := gitOutput(ctx, repoDir, "submodule", "foreach", "--recursive", "--quiet", "git status --porcelain=v1")
	if err != nil {
		return state, fmt.Errorf("inspect submodule source state: %w", err)
	}
	if len(submoduleStatus) != 0 {
		return state, fmt.Errorf("dirty submodule worktree is unsupported by source snapshot: %s", strings.TrimSpace(string(submoduleStatus)))
	}
	paths, err := untrackedPaths(ctx, repoDir)
	if err != nil {
		return state, err
	}
	for _, path := range paths {
		manifest, content, header, err := snapshotFile(repoDir, path)
		if err != nil {
			return state, err
		}
		state.untracked = append(state.untracked, sourceEntry{manifest: manifest, content: content, header: header})
	}
	h := sha256.New()
	_, _ = h.Write(state.status)
	_, _ = h.Write([]byte{0})
	_, _ = h.Write(state.patch)
	for _, entry := range state.untracked {
		_, _ = h.Write([]byte("\x00" + entry.manifest.Path + "\x00"))
		_, _ = h.Write(entry.content)
	}
	state.sha256 = hex.EncodeToString(h.Sum(nil))
	return state, nil
}

func untrackedPaths(ctx context.Context, repoDir string) ([]string, error) {
	cmd := exec.CommandContext(ctx, "git", "ls-files", "--others", "--exclude-standard", "-z")
	cmd.Dir = repoDir
	data, err := cmd.Output()
	if err != nil {
		return nil, fmt.Errorf("list untracked source: %w", err)
	}
	paths := strings.Split(string(data), "\x00")
	paths = paths[:len(paths)-1]
	sort.Strings(paths)
	return paths, nil
}

func snapshotFile(repoDir, path string) (SourceFile, []byte, *tar.Header, error) {
	fullPath := filepath.Join(repoDir, filepath.FromSlash(path))
	info, err := os.Lstat(fullPath)
	if err != nil {
		return SourceFile{}, nil, nil, fmt.Errorf("stat untracked source %s: %w", path, err)
	}
	linkTarget := ""
	var content []byte
	switch {
	case info.Mode().IsRegular():
		content, err = os.ReadFile(fullPath)
	case info.Mode()&os.ModeSymlink != 0:
		linkTarget, err = os.Readlink(fullPath)
		content = []byte(linkTarget)
	default:
		return SourceFile{}, nil, nil, fmt.Errorf("untracked source %s has unsupported mode %s", path, info.Mode())
	}
	if err != nil {
		return SourceFile{}, nil, nil, fmt.Errorf("read untracked source %s: %w", path, err)
	}
	header, err := tar.FileInfoHeader(info, linkTarget)
	if err != nil {
		return SourceFile{}, nil, nil, fmt.Errorf("describe untracked source %s: %w", path, err)
	}
	header.Name = filepath.ToSlash(path)
	sum := sha256.Sum256(content)
	return SourceFile{
		Path: path, Mode: uint32(info.Mode()), Size: info.Size(),
		SHA256: hex.EncodeToString(sum[:]), LinkTarget: linkTarget,
	}, content, header, nil
}

func sourceArtifact(path string, data []byte) SourceArtifact {
	sum := sha256.Sum256(data)
	return SourceArtifact{Path: path, SHA256: hex.EncodeToString(sum[:]), Size: int64(len(data))}
}

func sourceArtifactFromHash(path string, digest []byte, artifactPath string) SourceArtifact {
	info, _ := os.Stat(artifactPath)
	var size int64
	if info != nil {
		size = info.Size()
	}
	return SourceArtifact{Path: path, SHA256: hex.EncodeToString(digest), Size: size}
}

func Read(path string) (Report, error) {
	var report Report
	data, err := os.ReadFile(path)
	if err != nil {
		return report, err
	}
	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&report); err != nil {
		return report, fmt.Errorf("%s: %w", path, err)
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		return report, fmt.Errorf("%s: expected exactly one JSON object", path)
	}
	return report, nil
}

func ValidateReferenceReport(report Report, now time.Time) error {
	if err := ValidateReferenceEnvironment(report); err != nil {
		return err
	}
	return ValidateReferenceReportFreshness(report, now)
}

// ValidateReportIntegrity verifies the self-consistency shared by PASS and
// FAIL doctor reports. It deliberately does not decide reference suitability.
func ValidateReportIntegrity(report Report) error {
	if report.Version != 1 {
		return fmt.Errorf("doctor report version=%d, want 1", report.Version)
	}
	if report.GeneratedAt.IsZero() {
		return fmt.Errorf("doctor report generated_at is required")
	}
	if len(report.Checks) == 0 {
		return fmt.Errorf("doctor report contains no checks")
	}
	seen := make(map[string]bool, len(report.Checks))
	derivedOK := true
	for index, check := range report.Checks {
		name := strings.TrimSpace(check.Name)
		if name == "" || name != check.Name {
			return fmt.Errorf("doctor check[%d] has an empty or non-canonical name", index)
		}
		if seen[name] {
			return fmt.Errorf("doctor report contains duplicate check %q", name)
		}
		seen[name] = true
		switch check.Status {
		case StatusPass, StatusWarn:
		case StatusFail:
			derivedOK = false
		default:
			return fmt.Errorf("doctor check %q has invalid status %q", name, check.Status)
		}
	}
	if report.OK != derivedOK {
		return fmt.Errorf("doctor report ok=%v is inconsistent with check outcomes (derived ok=%v)", report.OK, derivedOK)
	}
	return nil
}

// ValidateReferenceReportStructure verifies the fixed shape and rig policy of
// a reference doctor report without requiring its checks to have passed.
func ValidateReferenceReportStructure(report Report) error {
	if err := ValidateReportIntegrity(report); err != nil {
		return err
	}
	if err := validateReferenceCheckSet(report.Checks); err != nil {
		return err
	}
	return validateReferenceRig(report.Rig)
}

// ValidateReferenceEnvironment verifies that a doctor report is a complete,
// passing reference-host snapshot and that the relevant host state still
// matches. Freshness is checked separately so resumable acquisitions can keep
// stale evidence while refusing to promote it.
func ValidateReferenceEnvironment(report Report) error {
	if err := ValidateReferenceReportStructure(report); err != nil {
		return err
	}
	if !report.OK {
		return fmt.Errorf("doctor report outcome is FAIL")
	}
	if report.Git.Dirty {
		return fmt.Errorf("reference measurement requires a clean source tree")
	}
	if !isGitCommit(report.Git.Commit) {
		return fmt.Errorf("reference measurement requires a recorded source commit")
	}
	if report.Git.PatchSHA256 != "" || report.Git.SourceSnapshot != nil {
		return fmt.Errorf("clean reference source provenance must not contain a dirty-worktree snapshot")
	}
	if err := validateReferenceCheckOutcomes(report.Checks); err != nil {
		return err
	}
	if !isSHA256(report.OrchestratorSHA256) {
		return fmt.Errorf("doctor report orchestrator sha256 is invalid")
	}
	if current := run.OrchestratorFingerprint(); current == "" || current != report.OrchestratorSHA256 {
		return fmt.Errorf("doctor report orchestrator does not match the running binary")
	}

	hostname, _ := os.Hostname()
	if hostname == "" || hostname != report.Hostname {
		return fmt.Errorf("doctor report hostname=%q does not match current host=%q", report.Hostname, hostname)
	}
	if report.Architecture == "" || report.Architecture != runtime.GOARCH {
		return fmt.Errorf("doctor report architecture=%q does not match current architecture=%q", report.Architecture, runtime.GOARCH)
	}
	currentKernel := readTrimmed("/proc/sys/kernel/osrelease")
	if report.KernelRelease == "" || report.KernelRelease != currentKernel {
		return fmt.Errorf("doctor report kernel_release=%q does not match current kernel=%q", report.KernelRelease, currentKernel)
	}
	if report.Clocksource == "" || report.Clocksource != report.Rig.ExpectedClocksource {
		return fmt.Errorf("doctor report clocksource=%q does not match reference rig=%q", report.Clocksource, report.Rig.ExpectedClocksource)
	}
	if current := readTrimmed("/sys/devices/system/clocksource/clocksource0/current_clocksource"); current == "" || current != report.Clocksource {
		return fmt.Errorf("clocksource changed after doctor: %q != %q", current, report.Clocksource)
	}
	if report.OnlineCPUs == "" || !rig.IsSubset(report.Rig.AllCPUs, report.OnlineCPUs) {
		return fmt.Errorf("doctor online CPU set %q does not contain reference rig %q", report.OnlineCPUs, report.Rig.AllCPUs)
	}
	if current := readTrimmed("/sys/devices/system/cpu/online"); current == "" || !equalCPUSet(current, report.OnlineCPUs) {
		return fmt.Errorf("online CPU set changed after doctor: %q != %q", current, report.OnlineCPUs)
	}
	if check := governorCheck(report.Rig, report.Governors); check.Status != StatusPass {
		return fmt.Errorf("doctor report governors do not satisfy reference rig: %s", check.Observed)
	}
	currentGovernors := readGovernors()
	if check := governorCheck(report.Rig, currentGovernors); check.Status != StatusPass {
		return fmt.Errorf("current governors do not satisfy reference rig: %s", check.Observed)
	}
	if !equalStringMap(report.Governors, currentGovernors) {
		return fmt.Errorf("CPU governors changed after doctor")
	}
	if report.NoFileSoft < report.Rig.MinNoFile {
		return fmt.Errorf("doctor nofile soft limit=%d is below reference minimum=%d", report.NoFileSoft, report.Rig.MinNoFile)
	}
	var currentLimit syscall.Rlimit
	if err := syscall.Getrlimit(syscall.RLIMIT_NOFILE, &currentLimit); err != nil {
		return fmt.Errorf("read current nofile limit: %w", err)
	}
	if report.NoFileSoft != currentLimit.Cur || report.NoFileHard != currentLimit.Max {
		return fmt.Errorf("nofile limit changed after doctor: soft/hard=%d/%d current=%d/%d",
			report.NoFileSoft, report.NoFileHard, currentLimit.Cur, currentLimit.Max)
	}
	return nil
}

// ValidateReferenceReportFreshness checks only the time window of an already
// validated reference environment.
func ValidateReferenceReportFreshness(report Report, now time.Time) error {
	if report.GeneratedAt.IsZero() || now.IsZero() || now.Sub(report.GeneratedAt) > 15*time.Minute || report.GeneratedAt.After(now.Add(time.Minute)) {
		return fmt.Errorf("doctor report must be generated within 15 minutes of reference sweep")
	}
	return nil
}

var referenceCheckNames = []string{
	"clocksource",
	"rig_cpu_layout",
	"bench_cpu_governor",
	"pid1_cpu_isolation",
	"slice_cpu_isolation_system",
	"slice_cpu_isolation_user",
	"slice_cpu_isolation_init.scope",
	"irq_cpu_isolation",
	"nofile",
	"residual_netem",
	"residual_benchmark_netns",
	"tool_ip",
	"tool_tc",
	"tool_ethtool",
	"tool_ping",
	"tool_iperf3",
	"tool_jq",
	"tool_sha256sum",
	"source_state",
}

func validateReferenceCheckSet(checks []Check) error {
	want := make(map[string]bool, len(referenceCheckNames))
	for _, name := range referenceCheckNames {
		want[name] = true
	}
	seen := make(map[string]bool, len(checks))
	for _, check := range checks {
		if !want[check.Name] {
			return fmt.Errorf("doctor report contains unexpected reference check %q", check.Name)
		}
		seen[check.Name] = true
	}
	for _, name := range referenceCheckNames {
		if !seen[name] {
			return fmt.Errorf("doctor report is missing required reference check %q", name)
		}
	}
	return nil
}

func validateReferenceCheckOutcomes(checks []Check) error {
	for _, check := range checks {
		if check.Status != StatusPass {
			return fmt.Errorf("reference doctor check %q is %s, want PASS", check.Name, check.Status)
		}
	}
	return nil
}

func validateReferenceRig(referenceRig rig.Rig) error {
	if strings.TrimSpace(referenceRig.Name) == "" {
		return fmt.Errorf("reference rig name is required")
	}
	if err := referenceRig.Validate(); err != nil {
		return fmt.Errorf("reference rig: %w", err)
	}
	if referenceRig.ExpectedClocksource == "" || !referenceRig.RequirePerformanceGovernor ||
		!referenceRig.RequireIsolation || referenceRig.MinNoFile == 0 {
		return fmt.Errorf("reference rig must require clocksource, performance governor, isolation, and nofile")
	}
	return nil
}

func isGitCommit(value string) bool {
	return (len(value) == 40 || len(value) == 64) && isLowerHex(value)
}

func isSHA256(value string) bool {
	return len(value) == sha256.Size*2 && isLowerHex(value)
}

func isLowerHex(value string) bool {
	for _, char := range value {
		if (char < '0' || char > '9') && (char < 'a' || char > 'f') {
			return false
		}
	}
	return true
}

func equalStringMap(left, right map[string]string) bool {
	if len(left) != len(right) {
		return false
	}
	for key, value := range left {
		if right[key] != value {
			return false
		}
	}
	return true
}

func governorCheck(r rig.Rig, governors map[string]string) Check {
	if r.ExpectFixedFrequency {
		bench, err := parseCPUSet(r.BenchCPUs)
		if err != nil {
			return Check{Name: "bench_cpu_governor", Status: StatusFail, Observed: err.Error(), Expected: "no cpufreq (fixed-frequency platform)"}
		}
		var present []string
		for _, cpu := range bench {
			name := strconv.Itoa(cpu)
			if governor, ok := governors[name]; ok {
				present = append(present, "cpu"+name+"="+governor)
			}
		}
		if len(present) > 0 {
			return Check{Name: "bench_cpu_governor", Status: StatusFail, Observed: strings.Join(present, ","), Expected: "no cpufreq (fixed-frequency platform)"}
		}
		return Check{Name: "bench_cpu_governor", Status: StatusPass, Observed: "no cpufreq", Expected: "no cpufreq (fixed-frequency platform)"}
	}
	if !r.RequirePerformanceGovernor {
		return Check{Name: "bench_cpu_governor", Status: StatusWarn, Detail: "rig does not require performance governor"}
	}
	bench, err := parseCPUSet(r.BenchCPUs)
	if err != nil {
		return Check{Name: "bench_cpu_governor", Status: StatusFail, Observed: err.Error(), Expected: "performance"}
	}
	var wrong, missing []string
	for _, cpu := range bench {
		name := strconv.Itoa(cpu)
		governor, ok := governors[name]
		switch {
		case !ok:
			missing = append(missing, name)
		case governor != "performance":
			wrong = append(wrong, "cpu"+name+"="+governor)
		}
	}
	if len(wrong) > 0 {
		return Check{Name: "bench_cpu_governor", Status: StatusFail, Observed: strings.Join(wrong, ","), Expected: "performance"}
	}
	if len(missing) == len(bench) {
		return Check{Name: "bench_cpu_governor", Status: StatusFail, Observed: "cpufreq unavailable", Expected: "performance"}
	}
	if len(missing) > 0 {
		return Check{Name: "bench_cpu_governor", Status: StatusFail, Observed: "missing cpu " + strings.Join(missing, ","), Expected: "performance"}
	}
	return Check{Name: "bench_cpu_governor", Status: StatusPass, Observed: "performance", Expected: "performance"}
}

func readGovernors() map[string]string {
	out := map[string]string{}
	paths, _ := filepath.Glob("/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor")
	for _, path := range paths {
		cpuDir := filepath.Base(filepath.Dir(filepath.Dir(path)))
		if strings.HasPrefix(cpuDir, "cpu") {
			out[strings.TrimPrefix(cpuDir, "cpu")] = readTrimmed(path)
		}
	}
	return out
}

func readTrimmed(path string) string {
	data, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(data))
}

func readStatusField(path, name string) string {
	for _, line := range strings.Split(readTrimmed(path), "\n") {
		key, value, ok := strings.Cut(line, ":")
		if ok && key == name {
			return strings.TrimSpace(value)
		}
	}
	return ""
}

func isBenchmarkNetworkNamespace(name string) bool {
	if strings.HasPrefix(name, "rudpbench") {
		return true
	}
	prefix, role, ok := strings.Cut(name, "-")
	if !ok || (role != "srv" && role != "cli") || len(prefix) != 11 || !strings.HasPrefix(prefix, "cm") {
		return false
	}
	return isLowerHex(prefix[2:])
}

func parseCPUSet(value string) ([]int, error) {
	return rig.ParseCPUSet(value)
}

func equalCPUSet(a, b string) bool {
	left, err := parseCPUSet(a)
	if err != nil {
		return false
	}
	right, err := parseCPUSet(b)
	if err != nil || len(left) != len(right) {
		return false
	}
	for i := range left {
		if left[i] != right[i] {
			return false
		}
	}
	return true
}

func benchCPUIRQs(benchCPUs string) []string {
	return irqAffinityConflicts("/proc/irq", benchCPUs)
}

func irqAffinityConflicts(irqRoot, benchCPUs string) []string {
	dirs, _ := filepath.Glob(filepath.Join(irqRoot, "[0-9]*"))
	var conflicts []string
	for _, dir := range dirs {
		info, err := os.Stat(dir)
		if err != nil || !info.IsDir() {
			continue
		}
		// smp_affinity_list is the requested mask. Managed and legacy IRQs can
		// expose a broad requested mask while the kernel routes them to a much
		// smaller effective set, so gate the CPUs that can actually receive it.
		affinityBytes, err := os.ReadFile(filepath.Join(dir, "effective_affinity_list"))
		if err != nil {
			affinityBytes, err = os.ReadFile(filepath.Join(dir, "smp_affinity_list"))
		}
		if err != nil {
			continue
		}
		affinity := strings.TrimSpace(string(affinityBytes))
		if affinity != "" && rig.Intersects(affinity, benchCPUs) {
			conflicts = append(conflicts, filepath.Base(dir)+":"+affinity)
		}
	}
	sort.Strings(conflicts)
	return conflicts
}

func commandOutput(ctx context.Context, dir, name string, args ...string) (string, error) {
	cmd := exec.CommandContext(ctx, name, args...)
	cmd.Dir = dir
	data, err := cmd.CombinedOutput()
	return strings.TrimSpace(string(data)), err
}

func gitOutput(ctx context.Context, dir string, args ...string) ([]byte, error) {
	cmd := exec.CommandContext(ctx, "git", args...)
	cmd.Dir = dir
	data, err := cmd.Output()
	if err != nil {
		if exitErr, ok := err.(*exec.ExitError); ok && len(exitErr.Stderr) > 0 {
			return nil, fmt.Errorf("%w: %s", err, strings.TrimSpace(string(exitErr.Stderr)))
		}
		return nil, err
	}
	return data, nil
}

func collectGit(ctx context.Context, dir string) (GitState, error) {
	state, err := readSourceState(ctx, dir)
	if err != nil {
		return GitState{}, err
	}
	gitState := GitState{Commit: state.commit, Dirty: len(state.status) != 0}
	if gitState.Dirty {
		gitState.PatchSHA256 = state.sha256
	}
	return gitState, nil
}
