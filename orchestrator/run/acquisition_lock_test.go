package run

import (
	"context"
	"errors"
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
