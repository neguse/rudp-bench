package cli

import (
	"errors"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
)

// FindRepoRoot walks up from the current working directory looking for a
// directory that contains both CMakeLists.txt and go.mod.
func FindRepoRoot() (string, error) {
	wd, err := os.Getwd()
	if err != nil {
		return "", err
	}
	dir, err := filepath.Abs(wd)
	if err != nil {
		return "", err
	}
	for {
		if FileExists(filepath.Join(dir, "CMakeLists.txt")) && FileExists(filepath.Join(dir, "go.mod")) {
			return dir, nil
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			return "", errors.New("could not find repository root")
		}
		dir = parent
	}
}

// FileExists reports whether path exists and is accessible.
func FileExists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}

// EnvDefault returns the value of the environment variable key if it is
// non-empty, or fallback otherwise.
func EnvDefault(key, fallback string) string {
	if value := os.Getenv(key); value != "" {
		return value
	}
	return fallback
}

// EnvBoolDefault returns the boolean value of the environment variable key.
// Recognised truthy values: 1, true, yes, on. Recognised falsy values: 0,
// false, no, off. An empty or unrecognised value returns fallback.
func EnvBoolDefault(key string, fallback bool) bool {
	value := os.Getenv(key)
	if value == "" {
		return fallback
	}
	switch strings.ToLower(value) {
	case "1", "true", "yes", "on":
		return true
	case "0", "false", "no", "off":
		return false
	default:
		return fallback
	}
}

// DefaultJobs returns the number of parallel jobs to use. It checks the JOBS
// environment variable first, falling back to runtime.NumCPU().
func DefaultJobs() int {
	if value := os.Getenv("JOBS"); value != "" {
		if parsed, err := strconv.Atoi(value); err == nil && parsed > 0 {
			return parsed
		}
	}
	return runtime.NumCPU()
}

// AbsPath returns path cleaned. If path is relative, it is joined with root
// first.
func AbsPath(root, path string) string {
	if filepath.IsAbs(path) {
		return filepath.Clean(path)
	}
	return filepath.Join(root, path)
}
