package run

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"hash"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
)

type commandIdentity struct {
	Command CommandConfig `json:"command"`
	SHA256  string        `json:"sha256,omitempty"`
}

// ConfigIdentity identifies an exact executable measurement treatment. Output
// location and the sweep's netem-gate reuse optimization do not affect it.
func ConfigIdentity(cfg RunConfig) string {
	canonical := cfg
	canonical.OutputDir = ""
	canonical.NetemGateOff = false
	record := struct {
		Config             RunConfig       `json:"config"`
		Server             commandIdentity `json:"server"`
		Client             commandIdentity `json:"client"`
		OrchestratorSHA256 string          `json:"orchestrator_sha256,omitempty"`
		EnvironmentSHA256  string          `json:"environment_sha256"`
	}{
		Config:             canonical,
		Server:             identifyCommand(canonical.ServerCommand),
		Client:             identifyCommand(canonical.ClientCommand),
		OrchestratorSHA256: OrchestratorFingerprint(),
		EnvironmentSHA256:  EnvironmentFingerprint(),
	}
	return HashValue(record)
}

func identifyCommand(command CommandConfig) commandIdentity {
	identity := commandIdentity{Command: command, SHA256: CommandFingerprint(command)}
	return identity
}

func CommandFingerprint(command CommandConfig) string {
	path := resolveCommandPath(command)
	if path == "" {
		return "unresolved"
	}
	h := sha256.New()
	if !hashFile(h, "command", path) {
		return "unreadable"
	}

	// A framework-dependent .NET build uses a stable native apphost while the
	// implementation lives in adjacent DLLs. Hash the runtime bundle whenever
	// the apphost's deps/runtimeconfig marker is present.
	dir := filepath.Dir(path)
	base := filepath.Base(path)
	if fileExists(filepath.Join(dir, base+".deps.json")) || fileExists(filepath.Join(dir, base+".runtimeconfig.json")) {
		entries, _ := os.ReadDir(dir)
		var dependencies []string
		for _, entry := range entries {
			if entry.IsDir() {
				continue
			}
			name := entry.Name()
			if strings.HasSuffix(name, ".dll") || strings.HasSuffix(name, ".json") ||
				strings.HasSuffix(name, ".so") || strings.Contains(name, ".so.") {
				dependencies = append(dependencies, name)
			}
		}
		sort.Strings(dependencies)
		for _, name := range dependencies {
			if !hashFile(h, "bundle/"+name, filepath.Join(dir, name)) {
				return "unreadable"
			}
		}
	}
	dependencies, resolved := dynamicDependencies(path)
	if !resolved {
		return "unreadable"
	}
	for _, dependency := range dependencies {
		if !hashFile(h, "shared/"+dependency, dependency) {
			return "unreadable"
		}
	}

	for _, arg := range command.Args {
		if path := resolveReferencedFile(command.Dir, arg); path != "" {
			if !hashFile(h, "arg/"+arg, path) {
				return "unreadable"
			}
		}
	}
	return hex.EncodeToString(h.Sum(nil))
}

func EnvironmentFingerprint() string {
	return HashValue(struct {
		Variables map[string]string `json:"variables"`
		Host      HostEnvironment   `json:"host"`
	}{RelevantEnvironment(), HostEnvironmentSnapshot()})
}

type HostEnvironment struct {
	Hostname        string            `json:"hostname,omitempty"`
	OS              string            `json:"os"`
	Architecture    string            `json:"architecture"`
	KernelRelease   string            `json:"kernel_release,omitempty"`
	MachineIDSHA256 string            `json:"machine_id_sha256,omitempty"`
	CPU             map[string]string `json:"cpu,omitempty"`
	OnlineCPUs      string            `json:"online_cpus,omitempty"`
	PresentCPUs     string            `json:"present_cpus,omitempty"`
	OnlineNUMANodes string            `json:"online_numa_nodes,omitempty"`
	Clocksource     string            `json:"clocksource,omitempty"`
	Governors       map[string]string `json:"governors,omitempty"`
}

func HostEnvironmentSnapshot() HostEnvironment {
	hostname, _ := os.Hostname()
	return HostEnvironment{
		Hostname:        hostname,
		OS:              runtime.GOOS,
		Architecture:    runtime.GOARCH,
		KernelRelease:   readIdentityFile("/proc/sys/kernel/osrelease"),
		MachineIDSHA256: fingerprintPath("/etc/machine-id"),
		CPU:             cpuIdentity(),
		OnlineCPUs:      readIdentityFile("/sys/devices/system/cpu/online"),
		PresentCPUs:     readIdentityFile("/sys/devices/system/cpu/present"),
		OnlineNUMANodes: readIdentityFile("/sys/devices/system/node/online"),
		Clocksource:     readIdentityFile("/sys/devices/system/clocksource/clocksource0/current_clocksource"),
		Governors:       identityGovernors(),
	}
}

func readIdentityFile(path string) string {
	data, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(data))
}

func cpuIdentity() map[string]string {
	data, err := os.ReadFile("/proc/cpuinfo")
	if err != nil {
		return nil
	}
	wanted := map[string]bool{
		"vendor_id": true, "cpu family": true, "model": true, "model name": true,
		"stepping": true, "microcode": true, "hardware": true, "revision": true,
	}
	out := map[string]string{}
	for _, line := range strings.Split(string(data), "\n") {
		if strings.TrimSpace(line) == "" && len(out) > 0 {
			break
		}
		key, value, ok := strings.Cut(line, ":")
		key = strings.TrimSpace(key)
		if ok && wanted[key] {
			out[key] = strings.TrimSpace(value)
		}
	}
	return out
}

func identityGovernors() map[string]string {
	paths, _ := filepath.Glob("/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor")
	out := map[string]string{}
	for _, path := range paths {
		cpu := filepath.Base(filepath.Dir(filepath.Dir(path)))
		out[cpu] = readIdentityFile(path)
	}
	return out
}

// RelevantEnvironment captures inherited variables with known runtime,
// allocator, loader, sanitizer, or benchmark behavior. Volatile session
// metadata is intentionally excluded; command-specific environment remains in
// CommandConfig and is hashed separately.
func RelevantEnvironment() map[string]string {
	out := map[string]string{}
	for _, entry := range os.Environ() {
		name, value, ok := strings.Cut(entry, "=")
		if ok && relevantEnvironmentName(name) {
			out[name] = value
		}
	}
	return out
}

func relevantEnvironmentName(name string) bool {
	for _, exact := range []string{
		"GODEBUG", "GOMAXPROCS", "GOMEMLIMIT", "GOGC", "GLIBC_TUNABLES",
		"OMP_NUM_THREADS", "RAYON_NUM_THREADS", "RUST_MIN_STACK",
	} {
		if name == exact {
			return true
		}
	}
	for _, prefix := range []string{
		"DOTNET_", "COMPlus_", "CORECLR_", "LD_", "MALLOC_", "GLIBC_",
		"ASAN_", "LSAN_", "MSAN_", "TSAN_", "UBSAN_", "OMP_", "BENCH_",
		"RUDP_", "QUIC_",
	} {
		if strings.HasPrefix(name, prefix) {
			return true
		}
	}
	return false
}

func OrchestratorFingerprint() string {
	path, err := os.Executable()
	if err != nil {
		return ""
	}
	return fingerprintPath(path)
}

func fingerprintPath(path string) string {
	if data, err := os.ReadFile(path); err == nil {
		sum := sha256.Sum256(data)
		return hex.EncodeToString(sum[:])
	}
	return ""
}

func resolveCommandPath(command CommandConfig) string {
	if command.Path == "" {
		return ""
	}
	if filepath.IsAbs(command.Path) {
		if fileExists(command.Path) {
			return command.Path
		}
		return ""
	}
	if command.Dir != "" {
		candidate := filepath.Join(command.Dir, command.Path)
		if fileExists(candidate) {
			return candidate
		}
	}
	if fileExists(command.Path) {
		return command.Path
	}
	if !strings.ContainsRune(command.Path, filepath.Separator) {
		if path, err := exec.LookPath(command.Path); err == nil {
			return path
		}
	}
	return ""
}

func resolveReferencedFile(dir, value string) string {
	if value == "" || strings.ContainsAny(value, "{}") {
		return ""
	}
	candidates := []string{value}
	if dir != "" && !filepath.IsAbs(value) {
		candidates = append([]string{filepath.Join(dir, value)}, candidates...)
	}
	for _, candidate := range candidates {
		info, err := os.Stat(candidate)
		if err == nil && info.Mode().IsRegular() {
			return candidate
		}
	}
	return ""
}

func fileExists(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.Mode().IsRegular()
}

func hashFile(h hash.Hash, label, path string) bool {
	data, err := os.ReadFile(path)
	if err != nil {
		return false
	}
	_, _ = h.Write([]byte(label))
	_, _ = h.Write([]byte{0})
	_, _ = h.Write(data)
	_, _ = h.Write([]byte{0})
	return true
}

func dynamicDependencies(path string) ([]string, bool) {
	absolute, err := filepath.Abs(path)
	if err != nil {
		return nil, false
	}
	data, err := exec.Command("ldd", absolute).Output()
	if err != nil {
		// Scripts and static binaries are fully represented by their own content.
		return nil, true
	}
	seen := map[string]bool{}
	for _, line := range strings.Split(string(data), "\n") {
		fields := strings.Fields(line)
		if len(fields) == 0 {
			continue
		}
		candidate := ""
		if len(fields) >= 3 && fields[1] == "=>" {
			if fields[2] == "not" {
				return nil, false
			}
			candidate = fields[2]
		} else if filepath.IsAbs(fields[0]) {
			candidate = fields[0]
		}
		if candidate != "" && fileExists(candidate) {
			seen[candidate] = true
		}
	}
	dependencies := make([]string, 0, len(seen))
	for path := range seen {
		dependencies = append(dependencies, path)
	}
	sort.Strings(dependencies)
	return dependencies, true
}

func HashValue(value any) string {
	data, err := json.Marshal(value)
	if err != nil {
		panic(err)
	}
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}

func HashBytes(data []byte) string {
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}
