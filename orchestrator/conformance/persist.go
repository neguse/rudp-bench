package conformance

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"reflect"
	"sort"
	"strings"

	"github.com/neguse/rudp-bench/orchestrator/run"
	"golang.org/x/sys/unix"
)

const (
	sessionStateDirectory = ".conformance"
	sessionPlanFile       = "plan.json"
	sessionReportFile     = "report.json"
	sessionManifestFile   = "manifest.json"
	sessionLockFile       = "lock"
	attemptsDirectory     = "attempts"
	attemptPlanFile       = "plan.json"
	attemptArtifactFile   = "artifact.json"
	attemptRecordFile     = "record.json"
	maxSessionJSONBytes   = 128 << 20
)

type sessionLock struct {
	file *os.File
}

func acquireSessionLock(outputDir string) (*sessionLock, error) {
	if err := ensureDirectory(outputDir); err != nil {
		return nil, fmt.Errorf("output_dir: %w", err)
	}
	stateDir := filepath.Join(outputDir, sessionStateDirectory)
	if err := ensureDirectory(stateDir); err != nil {
		return nil, fmt.Errorf("session state directory: %w", err)
	}
	path := filepath.Join(stateDir, sessionLockFile)
	if info, err := os.Lstat(path); err == nil {
		if info.Mode()&os.ModeSymlink != 0 || !info.Mode().IsRegular() {
			return nil, fmt.Errorf("session lock is not a regular file")
		}
	} else if !errors.Is(err, os.ErrNotExist) {
		return nil, fmt.Errorf("inspect session lock: %w", err)
	}
	file, err := os.OpenFile(path, os.O_CREATE|os.O_RDWR, 0o600)
	if err != nil {
		return nil, fmt.Errorf("open session lock: %w", err)
	}
	if err := unix.Flock(int(file.Fd()), unix.LOCK_EX|unix.LOCK_NB); err != nil {
		file.Close()
		if errors.Is(err, unix.EWOULDBLOCK) {
			return nil, fmt.Errorf("conformance session is already locked")
		}
		return nil, fmt.Errorf("lock conformance session: %w", err)
	}
	return &sessionLock{file: file}, nil
}

func (lock *sessionLock) close() error {
	if lock == nil || lock.file == nil {
		return nil
	}
	unlockErr := unix.Flock(int(lock.file.Fd()), unix.LOCK_UN)
	closeErr := lock.file.Close()
	if unlockErr != nil {
		return fmt.Errorf("unlock conformance session: %w", unlockErr)
	}
	return closeErr
}

func ensureDirectory(path string) error {
	info, err := os.Lstat(path)
	switch {
	case err == nil:
		if info.Mode()&os.ModeSymlink != 0 || !info.IsDir() {
			return fmt.Errorf("%s is not a directory", path)
		}
		return nil
	case !errors.Is(err, os.ErrNotExist):
		return err
	}
	parent := filepath.Dir(path)
	if parent != path {
		if err := ensureDirectory(parent); err != nil {
			return err
		}
	}
	if err := os.Mkdir(path, 0o755); err != nil && !errors.Is(err, os.ErrExist) {
		return err
	}
	info, err = os.Lstat(path)
	if err != nil {
		return err
	}
	if info.Mode()&os.ModeSymlink != 0 || !info.IsDir() {
		return fmt.Errorf("%s is not a directory", path)
	}
	return nil
}

func canonicalJSON(value any) ([]byte, error) {
	data, err := json.MarshalIndent(value, "", "  ")
	if err != nil {
		return nil, err
	}
	return append(data, '\n'), nil
}

func writeExclusiveCanonicalJSON(path string, value any) ([]byte, error) {
	data, err := canonicalJSON(value)
	if err != nil {
		return nil, fmt.Errorf("encode %s: %w", path, err)
	}
	if existing, err := readBoundedRegular(path, maxSessionJSONBytes); err == nil {
		if !bytes.Equal(existing, data) {
			return nil, fmt.Errorf("immutable session artifact changed: %s", path)
		}
		return existing, nil
	} else if !errors.Is(err, os.ErrNotExist) {
		return nil, fmt.Errorf("read %s: %w", path, err)
	}
	if err := atomicWrite(path, data); err != nil {
		return nil, err
	}
	return data, nil
}

func persistOrVerifySessionReport(path string, expected SessionReport) error {
	exists, err := fileExists(path)
	if err != nil {
		return fmt.Errorf("inspect session report: %w", err)
	}
	if !exists {
		_, err := writeExclusiveCanonicalJSON(path, expected)
		return err
	}
	var stored SessionReport
	if _, err := readCanonicalJSON(path, &stored); err != nil {
		return err
	}
	if !reflect.DeepEqual(stored, expected) {
		return fmt.Errorf("immutable session report changed: %s", path)
	}
	return nil
}

func atomicWrite(path string, data []byte) (returnErr error) {
	if err := ensureDirectory(filepath.Dir(path)); err != nil {
		return fmt.Errorf("create parent for %s: %w", path, err)
	}
	temporary, err := os.CreateTemp(filepath.Dir(path), ".atomic-")
	if err != nil {
		return fmt.Errorf("create temporary file for %s: %w", path, err)
	}
	temporaryPath := temporary.Name()
	defer func() {
		_ = temporary.Close()
		_ = os.Remove(temporaryPath)
	}()
	if err := temporary.Chmod(0o600); err != nil {
		return fmt.Errorf("chmod temporary file for %s: %w", path, err)
	}
	if _, err := temporary.Write(data); err != nil {
		return fmt.Errorf("write temporary file for %s: %w", path, err)
	}
	if err := temporary.Sync(); err != nil {
		return fmt.Errorf("fsync temporary file for %s: %w", path, err)
	}
	if err := temporary.Close(); err != nil {
		return fmt.Errorf("close temporary file for %s: %w", path, err)
	}
	if _, err := os.Lstat(path); err == nil {
		return fmt.Errorf("refusing to replace immutable session artifact %s", path)
	} else if !errors.Is(err, os.ErrNotExist) {
		return fmt.Errorf("inspect %s: %w", path, err)
	}
	if err := unix.Renameat2(unix.AT_FDCWD, temporaryPath, unix.AT_FDCWD, path, unix.RENAME_NOREPLACE); err != nil {
		return fmt.Errorf("rename temporary file to %s: %w", path, err)
	}
	directory, err := os.Open(filepath.Dir(path))
	if err != nil {
		return fmt.Errorf("open parent directory for %s: %w", path, err)
	}
	defer directory.Close()
	if err := directory.Sync(); err != nil {
		return fmt.Errorf("fsync parent directory for %s: %w", path, err)
	}
	return nil
}

func readCanonicalJSON(path string, target any) ([]byte, error) {
	data, err := readBoundedRegular(path, maxSessionJSONBytes)
	if err != nil {
		return nil, err
	}
	if err := strictDecode(data, target); err != nil {
		return nil, fmt.Errorf("decode %s: %w", path, err)
	}
	canonical, err := canonicalJSON(target)
	if err != nil {
		return nil, fmt.Errorf("canonicalize %s: %w", path, err)
	}
	if !bytes.Equal(data, canonical) {
		return nil, fmt.Errorf("session artifact is not canonical JSON: %s", path)
	}
	return data, nil
}

func readBoundedRegular(path string, limit int64) ([]byte, error) {
	info, err := os.Lstat(path)
	if err != nil {
		return nil, err
	}
	if info.Mode()&os.ModeSymlink != 0 || !info.Mode().IsRegular() {
		return nil, fmt.Errorf("%s is not a regular file", path)
	}
	if info.Size() > limit {
		return nil, fmt.Errorf("%s exceeds %d bytes", path, limit)
	}
	file, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer file.Close()
	data, err := io.ReadAll(io.LimitReader(file, limit+1))
	if err != nil {
		return nil, err
	}
	if int64(len(data)) > limit {
		return nil, fmt.Errorf("%s exceeds %d bytes", path, limit)
	}
	after, err := file.Stat()
	if err != nil {
		return nil, err
	}
	if !after.Mode().IsRegular() || after.Size() != int64(len(data)) || !os.SameFile(info, after) {
		return nil, fmt.Errorf("%s changed while being read", path)
	}
	return data, nil
}

func regularFileDigest(path string, limit int64) (uint64, string, error) {
	data, err := readBoundedRegular(path, limit)
	if err != nil {
		return 0, "", err
	}
	return uint64(len(data)), run.HashBytes(data), nil
}

func fileExists(path string) (bool, error) {
	_, err := os.Lstat(path)
	if err == nil {
		return true, nil
	}
	if errors.Is(err, os.ErrNotExist) {
		return false, nil
	}
	return false, err
}

func attemptStatePath(outputDir string, attempt AttemptPlan, name string) string {
	return filepath.Join(outputDir, sessionStateDirectory, attemptsDirectory,
		attempt.Transport, string(attempt.CaseID), fmt.Sprintf("attempt-%02d", attempt.AttemptNumber), name)
}

func validateSessionStateLayout(outputDir string, plan *SessionPlan) error {
	stateDir := filepath.Join(outputDir, sessionStateDirectory)
	allowedRoot := map[string]bool{
		sessionLockFile: true, sessionPlanFile: true, attemptsDirectory: true,
		sessionReportFile: true, sessionManifestFile: true,
	}
	allowedAttempts := map[string]AttemptPlan{}
	allowedDirectories := map[string]bool{attemptsDirectory: true}
	if plan != nil {
		for _, casePlan := range plan.Cases {
			for _, attempt := range casePlan.Attempts {
				relative := filepath.ToSlash(filepath.Join(attempt.Transport, string(attempt.CaseID), fmt.Sprintf("attempt-%02d", attempt.AttemptNumber)))
				allowedAttempts[relative] = attempt
				parts := strings.Split(relative, "/")
				for count := 1; count <= len(parts); count++ {
					allowedDirectories[filepath.ToSlash(filepath.Join(append([]string{attemptsDirectory}, parts[:count]...)...))] = true
				}
			}
		}
	}
	type attemptFiles struct {
		plan     bool
		artifact bool
		record   bool
	}
	filesByAttempt := map[string]attemptFiles{}
	attemptByRelative := map[string]AttemptPlan{}
	err := filepath.WalkDir(stateDir, func(path string, entry fs.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if path == stateDir {
			return nil
		}
		if entry.Type()&os.ModeSymlink != 0 {
			return fmt.Errorf("session state contains symlink %s", path)
		}
		relative, err := filepath.Rel(stateDir, path)
		if err != nil {
			return err
		}
		parts := strings.Split(filepath.ToSlash(relative), "/")
		if len(parts) == 1 {
			if !allowedRoot[parts[0]] {
				if strings.HasPrefix(parts[0], ".atomic-") && !entry.IsDir() {
					return os.Remove(path)
				}
				return fmt.Errorf("unknown session state entry %s", relative)
			}
			if parts[0] == attemptsDirectory && !entry.IsDir() {
				return fmt.Errorf("attempts state is not a directory")
			}
			if parts[0] != attemptsDirectory && entry.IsDir() {
				return fmt.Errorf("session state file is a directory: %s", relative)
			}
			return nil
		}
		if parts[0] != attemptsDirectory {
			return fmt.Errorf("unexpected nested session state entry %s", relative)
		}
		if plan == nil {
			return fmt.Errorf("attempt state exists before session plan")
		}
		if entry.IsDir() {
			if !allowedDirectories[filepath.ToSlash(relative)] {
				return fmt.Errorf("unknown attempt state directory %s", relative)
			}
			if len(parts) == 4 {
				attemptRelative := strings.Join(parts[1:4], "/")
				_, ok := allowedAttempts[attemptRelative]
				if !ok {
					return fmt.Errorf("unknown attempt state directory %s", relative)
				}
			}
			return nil
		}
		if len(parts) != 5 {
			return fmt.Errorf("unexpected attempt state file %s", relative)
		}
		attemptRelative := strings.Join(parts[1:4], "/")
		attempt, ok := allowedAttempts[attemptRelative]
		if !ok {
			return fmt.Errorf("unknown attempt state file %s", relative)
		}
		state := filesByAttempt[attemptRelative]
		switch parts[4] {
		case attemptPlanFile:
			state.plan = true
		case attemptArtifactFile:
			state.artifact = true
		case attemptRecordFile:
			state.record = true
		default:
			if strings.HasPrefix(parts[4], ".atomic-") {
				return os.Remove(path)
			}
			return fmt.Errorf("unknown attempt state file %s", relative)
		}
		filesByAttempt[attemptRelative] = state
		attemptByRelative[attemptRelative] = attempt
		return nil
	})
	if err != nil {
		return err
	}
	consumedByCase := map[string][]int{}
	for relative, state := range filesByAttempt {
		attempt := attemptByRelative[relative]
		if (state.artifact || state.record) && !state.plan {
			return fmt.Errorf("attempt state %s has artifact/record before plan receipt", relative)
		}
		if state.plan {
			key := attempt.Transport + "/" + string(attempt.CaseID)
			consumedByCase[key] = append(consumedByCase[key], attempt.AttemptNumber)
		}
	}
	for key, attempts := range consumedByCase {
		sort.Ints(attempts)
		for index, number := range attempts {
			if number != index+1 {
				return fmt.Errorf("attempt state gap for %s: found attempt %d without complete prefix", key, number)
			}
		}
	}
	return nil
}

type SessionManifestEntry struct {
	Path      string `json:"path"`
	SizeBytes uint64 `json:"size_bytes"`
	SHA256    string `json:"sha256"`
}

type SessionManifest struct {
	Version         int                    `json:"version"`
	SessionIdentity string                 `json:"session_identity"`
	Directories     []string               `json:"directories,omitempty"`
	Entries         []SessionManifestEntry `json:"entries"`
}

func buildSessionManifest(outputDir, sessionIdentity string) (SessionManifest, error) {
	manifest := SessionManifest{Version: 1, SessionIdentity: sessionIdentity}
	err := filepath.WalkDir(outputDir, func(path string, entry fs.DirEntry, walkErr error) error {
		if walkErr != nil {
			return walkErr
		}
		if path == outputDir {
			return nil
		}
		if entry.Type()&os.ModeSymlink != 0 {
			return fmt.Errorf("session output contains symlink %s", path)
		}
		relative, err := filepath.Rel(outputDir, path)
		if err != nil {
			return err
		}
		relative = filepath.ToSlash(relative)
		if entry.IsDir() {
			manifest.Directories = append(manifest.Directories, relative)
			return nil
		}
		if !entry.Type().IsRegular() {
			return fmt.Errorf("session output contains non-regular artifact %s", path)
		}
		// Only the self-referential manifest is excluded. The advisory lock is
		// inventoried because flock changes kernel state, not file contents.
		if relative == filepath.ToSlash(filepath.Join(sessionStateDirectory, sessionManifestFile)) {
			return nil
		}
		info, err := entry.Info()
		if err != nil {
			return err
		}
		digest, err := hashRegularFile(path, info)
		if err != nil {
			return err
		}
		manifest.Entries = append(manifest.Entries, SessionManifestEntry{
			Path: relative, SizeBytes: uint64(info.Size()), SHA256: digest,
		})
		return nil
	})
	if err != nil {
		return SessionManifest{}, err
	}
	sort.Strings(manifest.Directories)
	sort.Slice(manifest.Entries, func(i, j int) bool { return manifest.Entries[i].Path < manifest.Entries[j].Path })
	return manifest, nil
}

func hashRegularFile(path string, before fs.FileInfo) (string, error) {
	file, err := os.Open(path)
	if err != nil {
		return "", err
	}
	hash := sha256.New()
	_, copyErr := io.Copy(hash, file)
	after, statErr := file.Stat()
	closeErr := file.Close()
	if copyErr != nil {
		return "", copyErr
	}
	if statErr != nil {
		return "", statErr
	}
	if closeErr != nil {
		return "", closeErr
	}
	if !after.Mode().IsRegular() || before.Size() != after.Size() || !os.SameFile(before, after) {
		return "", fmt.Errorf("artifact changed while hashing: %s", path)
	}
	return hex.EncodeToString(hash.Sum(nil)), nil
}

func verifySessionManifest(outputDir string, expected SessionManifest) error {
	if expected.Version != 1 || !isSHA256Digest(expected.SessionIdentity) {
		return fmt.Errorf("invalid session manifest identity/version")
	}
	seen := map[string]bool{}
	seenDirectories := map[string]bool{}
	for index, directory := range expected.Directories {
		if directory == "" || filepath.IsAbs(directory) || filepath.ToSlash(filepath.Clean(directory)) != directory || strings.HasPrefix(directory, "../") {
			return fmt.Errorf("manifest directory[%d] has unsafe path %q", index, directory)
		}
		if seenDirectories[directory] {
			return fmt.Errorf("manifest contains duplicate directory %q", directory)
		}
		seenDirectories[directory] = true
		if index > 0 && expected.Directories[index-1] >= directory {
			return fmt.Errorf("manifest directories are not strictly sorted")
		}
	}
	for index, entry := range expected.Entries {
		if entry.Path == "" || filepath.IsAbs(entry.Path) || filepath.ToSlash(filepath.Clean(entry.Path)) != entry.Path || strings.HasPrefix(entry.Path, "../") {
			return fmt.Errorf("manifest entry[%d] has unsafe path %q", index, entry.Path)
		}
		if seen[entry.Path] {
			return fmt.Errorf("manifest contains duplicate path %q", entry.Path)
		}
		seen[entry.Path] = true
		if !isSHA256Digest(entry.SHA256) {
			return fmt.Errorf("manifest entry %q has invalid sha256", entry.Path)
		}
		if index > 0 && expected.Entries[index-1].Path >= entry.Path {
			return fmt.Errorf("manifest entries are not strictly sorted")
		}
	}
	actual, err := buildSessionManifest(outputDir, expected.SessionIdentity)
	if err != nil {
		return err
	}
	if !reflect.DeepEqual(actual, expected) {
		return fmt.Errorf("session manifest does not match current output tree")
	}
	return nil
}

func validateManifestReceipts(outputDir string, manifest SessionManifest, plan SessionPlan,
	recordsByTransport map[string][]AttemptRecord,
) error {
	entries := make(map[string]SessionManifestEntry, len(manifest.Entries))
	for _, entry := range manifest.Entries {
		entries[entry.Path] = entry
	}
	attempts := make(map[string]AttemptPlan)
	for _, casePlan := range plan.Cases {
		for _, attempt := range casePlan.Attempts {
			attempts[attempt.SlotIdentity] = attempt
		}
	}
	seenRecords := map[string]bool{}
	for _, records := range recordsByTransport {
		for _, record := range records {
			if seenRecords[record.SlotIdentity] {
				return fmt.Errorf("manifest receipt check found duplicate attempt record %s", record.SlotIdentity)
			}
			seenRecords[record.SlotIdentity] = true
			attempt, ok := attempts[record.SlotIdentity]
			if !ok {
				return fmt.Errorf("manifest receipt check found unknown attempt record %s", record.SlotIdentity)
			}
			if record.Artifact != nil {
				artifact := *record.Artifact
				if artifact.Version != 1 || artifact.SlotIdentity != attempt.SlotIdentity ||
					artifact.RelativePath != "result.json" || !isSHA256Digest(artifact.ResultSHA256) {
					return fmt.Errorf("manifest receipt check found invalid result receipt for %s", record.SlotIdentity)
				}
				path, err := sessionManifestPath(outputDir, attempt.RunConfig.OutputDir, artifact.RelativePath)
				if err != nil {
					return fmt.Errorf("manifest result receipt for %s: %w", record.SlotIdentity, err)
				}
				if err := matchManifestReceipt(entries, path, artifact.SizeBytes, artifact.ResultSHA256); err != nil {
					return fmt.Errorf("manifest result receipt for %s: %w", record.SlotIdentity, err)
				}
			}
			if record.Extraction == nil || record.Extraction.Evidence == nil {
				continue
			}
			evidence := record.Extraction.Evidence
			if record.Artifact == nil || evidence.ResultSHA256 != record.Artifact.ResultSHA256 {
				return fmt.Errorf("manifest metrics receipt for %s is not bound to its result receipt", record.SlotIdentity)
			}
			seenMetrics := map[string]bool{}
			for index, metric := range evidence.MetricsArtifacts {
				path, err := sessionManifestPath(outputDir, attempt.RunConfig.OutputDir, metric.RelativePath)
				if err != nil {
					return fmt.Errorf("manifest metrics receipt for %s[%d]: %w", record.SlotIdentity, index, err)
				}
				if seenMetrics[path] {
					return fmt.Errorf("manifest metrics receipt for %s duplicates %s", record.SlotIdentity, path)
				}
				seenMetrics[path] = true
				if !isSHA256Digest(metric.SHA256) {
					return fmt.Errorf("manifest metrics receipt for %s[%d] has invalid sha256", record.SlotIdentity, index)
				}
				if err := matchManifestReceipt(entries, path, metric.SizeBytes, metric.SHA256); err != nil {
					return fmt.Errorf("manifest metrics receipt for %s[%d]: %w", record.SlotIdentity, index, err)
				}
			}
		}
	}
	return nil
}

func sessionManifestPath(outputDir, attemptOutputDir, relative string) (string, error) {
	if relative == "" || filepath.IsAbs(relative) || filepath.ToSlash(relative) != relative ||
		filepath.ToSlash(filepath.Clean(filepath.FromSlash(relative))) != relative ||
		relative == ".." || strings.HasPrefix(relative, "../") {
		return "", fmt.Errorf("unsafe artifact path %q", relative)
	}
	target := filepath.Join(attemptOutputDir, filepath.FromSlash(relative))
	sessionRelative, err := filepath.Rel(outputDir, target)
	if err != nil {
		return "", err
	}
	if sessionRelative == "." || sessionRelative == ".." || strings.HasPrefix(sessionRelative, ".."+string(filepath.Separator)) {
		return "", fmt.Errorf("artifact path escapes session output: %q", relative)
	}
	return filepath.ToSlash(sessionRelative), nil
}

func matchManifestReceipt(entries map[string]SessionManifestEntry, path string, size uint64, digest string) error {
	entry, ok := entries[path]
	if !ok {
		return fmt.Errorf("manifest is missing %s", path)
	}
	if entry.SizeBytes != size || entry.SHA256 != digest {
		return fmt.Errorf("manifest entry %s does not match immutable receipt", path)
	}
	return nil
}

func hashCanonicalBytes(data []byte) string {
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}
