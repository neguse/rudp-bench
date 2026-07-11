package run

import (
	"context"
	"errors"
	"fmt"
	"time"

	"golang.org/x/sys/unix"
)

const acquisitionLockPath = "/tmp/rudp-bench-acquisition.lock"

type acquisitionLock struct {
	fd int
}

func acquireAcquisitionLock(ctx context.Context) (*acquisitionLock, error) {
	return acquireAcquisitionLockAt(ctx, acquisitionLockPath)
}

func acquireAcquisitionLockAt(ctx context.Context, path string) (*acquisitionLock, error) {
	fd, err := unix.Open(path, unix.O_RDONLY|unix.O_CREAT|unix.O_CLOEXEC|unix.O_NOFOLLOW, 0o666)
	if err != nil {
		return nil, fmt.Errorf("open rig-global acquisition lock: %w", err)
	}
	closeOnError := func(err error) (*acquisitionLock, error) {
		_ = unix.Close(fd)
		return nil, err
	}
	var stat unix.Stat_t
	if err := unix.Fstat(fd, &stat); err != nil {
		return closeOnError(fmt.Errorf("stat rig-global acquisition lock: %w", err))
	}
	if stat.Mode&unix.S_IFMT != unix.S_IFREG || stat.Nlink != 1 {
		return closeOnError(fmt.Errorf("rig-global acquisition lock is not a single-link regular file"))
	}

	ticker := time.NewTicker(100 * time.Millisecond)
	defer ticker.Stop()
	for {
		if err := unix.Flock(fd, unix.LOCK_EX|unix.LOCK_NB); err == nil {
			return &acquisitionLock{fd: fd}, nil
		} else if !errors.Is(err, unix.EWOULDBLOCK) && !errors.Is(err, unix.EAGAIN) {
			return closeOnError(fmt.Errorf("lock rig-global acquisition: %w", err))
		}
		select {
		case <-ctx.Done():
			return closeOnError(fmt.Errorf("wait for rig-global acquisition lock: %w", ctx.Err()))
		case <-ticker.C:
		}
	}
}

func (lock *acquisitionLock) close() error {
	if lock == nil || lock.fd < 0 {
		return nil
	}
	fd := lock.fd
	lock.fd = -1
	unlockErr := unix.Flock(fd, unix.LOCK_UN)
	closeErr := unix.Close(fd)
	if unlockErr != nil {
		unlockErr = fmt.Errorf("unlock rig-global acquisition: %w", unlockErr)
	}
	if closeErr != nil {
		closeErr = fmt.Errorf("close rig-global acquisition lock: %w", closeErr)
	}
	return errors.Join(unlockErr, closeErr)
}
