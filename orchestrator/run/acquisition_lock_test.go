package run

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestAcquisitionLockSerializesAndHonorsCancellation(t *testing.T) {
	path := filepath.Join(t.TempDir(), "acquisition.lock")
	first, err := acquireAcquisitionLockAt(context.Background(), path)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Millisecond)
	defer cancel()
	if _, err := acquireAcquisitionLockAt(ctx, path); !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("contended lock error=%v, want deadline", err)
	}
	if err := first.close(); err != nil {
		t.Fatal(err)
	}
	second, err := acquireAcquisitionLockAt(context.Background(), path)
	if err != nil {
		t.Fatalf("acquire after release: %v", err)
	}
	if err := second.close(); err != nil {
		t.Fatal(err)
	}
}

func TestAcquisitionLockOpensPreexistingFileWithoutCreate(t *testing.T) {
	path := filepath.Join(t.TempDir(), "acquisition.lock")
	// A read-only pre-existing lock rejects O_CREAT-with-write paths and
	// approximates fs.protected_regular denying O_CREAT on another user's file.
	if err := os.WriteFile(path, nil, 0o444); err != nil {
		t.Fatal(err)
	}
	lock, err := acquireAcquisitionLockAt(context.Background(), path)
	if err != nil {
		t.Fatalf("acquire pre-existing lock: %v", err)
	}
	if err := lock.close(); err != nil {
		t.Fatal(err)
	}
}
